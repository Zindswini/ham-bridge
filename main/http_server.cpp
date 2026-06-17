#include "http_server.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <esp_err.h>
#include <esp_eth_com.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_https_server.h>
#include <esp_log.h>
#include <esp_system.h>
#include <memory>
#include <sys/cdefs.h>

#include "config.h"
#include "esp_netif_types.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"

#include "key_cert_manager.h"
#include "wss_keep_alive.h"

// Implementation Reference:
// https://github.com/espressif/esp-idf/blob/v6.0.1/examples/protocols/https_server/wss_server/main/wss_server_example.c

static const char *tag = "HTTPS_WSS_SERVER";
static httpd_handle_t server = nullptr;

static esp_err_t wsHandler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    ESP_LOGI(tag, "Handshake done, new websocket connection opened");
    return ESP_OK;
  }

  httpd_ws_frame_t ws_pkt;
  uint8_t *buf = nullptr;
  memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

  // Call receive w/ len 0 to extract just payload size
  esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
  if (ret != ESP_OK) {
    ESP_LOGE(tag, "httpd_ws_recv_frame failed to get frame length with %d",
             ret);
    return ret;
  }

  ESP_LOGI(tag, "Frame length is %d", ws_pkt.len);
  if (ws_pkt.len != 0) {
    // Allocate memory for the payload
    buf = static_cast<uint8_t *>(calloc(1, (ws_pkt.len + 1)));
    if (buf == nullptr) {
      ESP_LOGE(tag, "Failed to calloc memory for buf with len %d",
               ws_pkt.len + 1);
      return ESP_ERR_NO_MEM;
    }

    ws_pkt.payload = buf;
    // Call receive w/ actual len, writes into allocated memory
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
      ESP_LOGE(tag, "httpd_ws_recv_frame failed with %d", ret);
      free(buf);
      return ret;
    }
  }

  // Now have payload written to ws_pkt

  // Handle PONG message
  if (ws_pkt.type == HTTPD_WS_TYPE_PONG) {
    ESP_LOGD(tag, "Received PONG message");
    free(buf);
    return wss_keep_alive_client_is_active(
        static_cast<wss_keep_alive_t>(httpd_get_global_user_ctx(req->handle)),
        httpd_req_to_sockfd(req));
  }

  // If TEXT message, echo it back (for now) TODO:
  if (ws_pkt.type == HTTPD_WS_TYPE_TEXT || ws_pkt.type == HTTPD_WS_TYPE_PING ||
      ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
      ESP_LOGI(tag, "Received packet with message %s", ws_pkt.payload);
    } else if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
      ESP_LOGI(tag, "Got WS PING frame, replying w/ PONG");
      ws_pkt.type = HTTPD_WS_TYPE_PONG;
    } else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
      ws_pkt.len = 0;
      ws_pkt.payload = nullptr;
    }
    ret = httpd_ws_send_frame(req, &ws_pkt);
    if (ret != ESP_OK) {
      ESP_LOGE(tag, "httpd_ws_send_frame failed with %d", ret);
    }

    ESP_LOGI(tag, "ws_handler: httpd_handle_t=%p, sockfd=%d, client_info=%d",
             req->handle, httpd_req_to_sockfd(req),
             httpd_ws_get_fd_info(req->handle, httpd_req_to_sockfd(req)));
    free(buf);
    return ret;
  }

  free(buf);
  return ESP_OK;
}

esp_err_t wssOpenFd(httpd_handle_t httpd_handle, int sockfd) {
  ESP_LOGI(tag, "New client connected %d", sockfd);

  auto *keep_alive_inst =
      static_cast<wss_keep_alive_t>(httpd_get_global_user_ctx(httpd_handle));
  return wss_keep_alive_add_client(keep_alive_inst, sockfd);
}

void wssCloseFd(httpd_handle_t httpd_handle, int sockfd) {
  ESP_LOGI(tag, "Client disconnected %d", sockfd);

  auto *keep_alive_inst =
      static_cast<wss_keep_alive_t>(httpd_get_global_user_ctx(httpd_handle));
  wss_keep_alive_remove_client(keep_alive_inst, sockfd);
  fclose((FILE *)sockfd);
}

static const httpd_uri_t kWsUriConf = {
    .uri = "/ws",
    .method = HTTP_GET,
    .handler = wsHandler,
    .user_ctx = NULL,
    .is_websocket = true,
    .handle_ws_control_frames = true,
    .supported_subprotocol = nullptr,
};

static void sendHello(void *arg) {
  static const char *data = "Hello client!";
  auto *resp_arg = static_cast<AsyncRespArg *>(arg);

  httpd_handle_t httpd_handle = resp_arg->hd;
  int file_desc = resp_arg->fd;
  httpd_ws_frame_t ws_pkt;
  memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

  ws_pkt.payload = (uint8_t *)data;
  ws_pkt.len = strlen(data);
  ws_pkt.type = HTTPD_WS_TYPE_TEXT;

  httpd_ws_send_frame_async(httpd_handle, file_desc, &ws_pkt);
  free(resp_arg);
}

static void sendPing(void *arg) {
  auto *resp_arg = static_cast<AsyncRespArg *>(arg);

  httpd_handle_t httpd_handle = resp_arg->hd;
  int file_desc = resp_arg->fd;
  httpd_ws_frame_t ws_pkt;
  memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

  ws_pkt.payload = nullptr;
  ws_pkt.len = 0;
  ws_pkt.type = HTTPD_WS_TYPE_PING;

  httpd_ws_send_frame_async(httpd_handle, file_desc, &ws_pkt);
  free(resp_arg);
}

bool clientNotAliveCallback(wss_keep_alive_t keep_alive_handle, int file_desc) {
  ESP_LOGE(tag, "Client not alive, closing fd %d", file_desc);
  esp_err_t ret = httpd_sess_trigger_close(
      wss_keep_alive_get_user_ctx(keep_alive_handle), file_desc);
  if (ret != ESP_OK) {
    ESP_LOGE(tag, "Failed to close wss fd %d", file_desc);
    return false;
  }
  return true;
}

bool checkClientAliveCallback(wss_keep_alive_t keep_alive_handle,
                              int file_desc) {
  ESP_LOGD(tag, "Checking if client (fd=%d) is alive", file_desc);

  auto *resp_arg =
      static_cast<AsyncRespArg *>(malloc(sizeof(struct AsyncRespArg)));
  if (resp_arg == nullptr) {
    ESP_LOGE(tag, "Failed to allocate memory for wss healthcheck on fd %d",
             file_desc);
    assert(resp_arg != nullptr);
  }
  resp_arg->hd = wss_keep_alive_get_user_ctx(keep_alive_handle);
  resp_arg->fd = file_desc;

  return httpd_queue_work(resp_arg->hd, sendPing, resp_arg) == ESP_OK;
}

static void startWssEchoServer() {
  // Prepare keep alive engine
  wss_keep_alive_config_t keep_alive_config = KEEP_ALIVE_CONFIG_DEFAULT();
  keep_alive_config.max_clients = MAX_HTTPS_CLIENTS;
  keep_alive_config.client_not_alive_cb = clientNotAliveCallback;
  keep_alive_config.check_client_alive_cb = checkClientAliveCallback;

  wss_keep_alive_t keep_alive = wss_keep_alive_start(&keep_alive_config);

  ESP_LOGI(tag, "Starting HTTPS Websocket Server");
  esp_err_t ret = 0;

  httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
  conf.httpd.max_open_sockets = MAX_HTTPS_CLIENTS;
  conf.httpd.global_user_ctx = keep_alive;
  conf.httpd.open_fn = wssOpenFd;
  conf.httpd.close_fn = wssCloseFd;

  // Ask key_cert_manager to attach cert pointers to config
  ESP_ERROR_CHECK(attachKeyCertBundleToConfig(&conf));

  ret = httpd_ssl_start(&server, &conf);
  if (ret != ESP_OK) {
    ESP_LOGE(tag, "Error starting httpd ssl server: %d", ret);
    return;
  }
  ESP_LOGI(tag, "Started httpd ssl server successfully");

  ESP_LOGI(tag, "Registering URI Handlers");
  httpd_register_uri_handler(server, &kWsUriConf);
  wss_keep_alive_set_user_ctx(keep_alive, server);
}

static esp_err_t stopWssEchoServer() {
  wss_keep_alive_stop(
      static_cast<wss_keep_alive_t>(httpd_get_global_user_ctx(server)));
  return httpd_ssl_stop(server);
}

static void disconnectHandler(void *arg __unused,
                              esp_event_base_t event_base __unused,
                              int32_t event_id __unused,
                              void *event_data __unused) {
  if (server != nullptr) {
    esp_err_t ret = stopWssEchoServer();
    if (ret == ESP_OK) {
      server = nullptr;
      ESP_LOGI(tag, "Successfully stopped wss echo server");
    } else {
      ESP_LOGE(tag, "Failed to stop wss echo server with error %d", ret);
    }
  } else {
    ESP_LOGE(tag, "disconnectHandler called but server ref is nullptr!");
  }
}

static void connectHandler(void *arg __unused,
                           esp_event_base_t event_base __unused,
                           int32_t event_id __unused,
                           void *event_data __unused) {
  if (server == nullptr) {
    ESP_LOGI(tag, "Server setup handler called, starting server");
    startWssEchoServer();
  } else {
    ESP_LOGE(tag, "connectHandler called but server address was not null!");
  }
}

static void wssServerSendMessages() {
  bool send_messages = true;

  while (send_messages) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (server == nullptr) {
      ESP_LOGD(tag, "SendMessages looping, server ref is nullptr");
      continue; // Server might not be created
    }

    size_t clients = MAX_HTTPS_CLIENTS;
    std::array<int, MAX_HTTPS_CLIENTS> client_fds{};

    if (httpd_get_client_list(server, &clients, client_fds.data()) == ESP_OK) {
      for (size_t i = 0; i < clients; i++) {
        int sock = client_fds.at(i);
        if (httpd_ws_get_fd_info(server, sock) == HTTPD_WS_CLIENT_WEBSOCKET) {
          ESP_LOGI(tag, "Active client: fd=%d -> sending async message", sock);
          auto resp_arg = std::shared_ptr<AsyncRespArg>();
          assert(resp_arg != nullptr);
          resp_arg->hd = server;
          resp_arg->fd = sock;
          if (httpd_queue_work(resp_arg->hd, sendHello, resp_arg.get()) !=
              ESP_OK) {
            ESP_LOGE(tag, "httpd_queue_work failed");
            send_messages = false;
            break;
          }
        }
      }
    } else {
      ESP_LOGE(tag, "httpd_get_client_list failed!");
      return;
    }
  }
}

extern "C" void wssServerTask() {
  psa_crypto_init();

  ESP_LOGI(tag, "Registering connect/disconnect handlers");
  // Register server start/stop functions
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                             &connectHandler, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(
      ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, &disconnectHandler, nullptr));

  ESP_LOGI(tag, "Registered connect/disconnect handlers");

  wssServerSendMessages();
}