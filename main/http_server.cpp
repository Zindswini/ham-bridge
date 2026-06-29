#include "http_server.h"

#include <cstddef>
#include <esp_err.h>
#include <esp_eth_com.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_https_server.h>
#include <esp_log.h>
#include <esp_system.h>
#include <memory>
#include <vector>

#include "config.h"
#include "esp_netif_types.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"

#include "key_cert_manager.h"
#include "portmacro.h"

// Implementation Reference:
// https://github.com/espressif/esp-idf/blob/v6.0.1/examples/protocols/https_server/wss_server/main/wss_server_example.c

static const char *tag = "HTTPS_WSS_SERVER";
static httpd_handle_t server = nullptr;

static esp_err_t wsHandler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    ESP_LOGI(tag, "Handshake done, new websocket connection opened");
    return ESP_OK;
  }

  httpd_ws_frame_t ws_pkt = {
      .final = true,
      .fragmented = false, // final must be true
      .type = HTTPD_WS_TYPE_CONTINUE,
      .payload = nullptr,
      .len = 0,
  };
  std::vector<uint8_t> buf;

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
    buf.resize(ws_pkt.len);

    ws_pkt.payload = buf.data();
    // Call receive w/ actual len, writes into allocated memory
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
      ESP_LOGE(tag, "httpd_ws_recv_frame failed with %d", ret);
      return ret;
    }
  }

  // Now have metadata and payload written to ws_pkt and buf
  switch (ws_pkt.type) {
  case HTTPD_WS_TYPE_PONG:
    ESP_LOGD(tag, "Received PONG message");
    // TODO(Zindswini): Actually mark connection as healthy
    break;
  case HTTPD_WS_TYPE_TEXT:
  case HTTPD_WS_TYPE_PING:
  case HTTPD_WS_TYPE_CLOSE:
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
    return ret;

    break;
  case HTTPD_WS_TYPE_CONTINUE:
  case HTTPD_WS_TYPE_BINARY:
    ESP_LOGI(tag, "HTTP Payload type has no implementation!");
    break;
  }

  return ESP_OK;
}

static const httpd_uri_t kWsUriConf = {
    .uri = "/ws",
    .method = HTTP_GET,
    .handler = wsHandler,
    .user_ctx = nullptr,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = nullptr,
};

static void sendHello(void *arg) {
  std::string data_string = std::string("Hello Client!");
  std::vector<uint8_t> data_vec(data_string.begin(), data_string.end());

  // Take ownership of data at arg (cleaned up automatically)
  auto resp_arg =
      std::unique_ptr<AsyncRespArg>(static_cast<AsyncRespArg *>(arg));

  httpd_ws_frame_t ws_pkt = {
      .final = true,
      .fragmented = false,
      .type = HTTPD_WS_TYPE_TEXT,
      .payload = data_vec.data(),
      .len = data_vec.size(),
  };

  esp_err_t ret =
      httpd_ws_send_frame_async(resp_arg->hd, resp_arg->fd, &ws_pkt);
  if (ret != ESP_OK) {
    ESP_LOGE(tag, "Failed to send websocket frame! ret = %d", ret);
  }
}

static void sendPing(void *arg) {
  auto *resp_arg = static_cast<AsyncRespArg *>(arg);

  httpd_ws_frame_t ws_pkt = {.final = true,
                             .fragmented = false,
                             .type = HTTPD_WS_TYPE_PING,
                             .payload = nullptr,
                             .len = 0};

  esp_err_t ret =
      httpd_ws_send_frame_async(resp_arg->hd, resp_arg->fd, &ws_pkt);
  if (ret != ESP_OK) {
    ESP_LOGE(tag, "Failed to send websocket frame! ret = %d", ret);
  }
}

esp_err_t wssOpenFd(httpd_handle_t handle __unused, int sockfd) {
  ESP_LOGI(tag, "New httpd connection with sockfd %d", sockfd);
  return ESP_OK;
}

static void startWssServer() {
  // Prepare web socket server
  esp_err_t ret = ESP_OK;
  if (ENABLE_SSL) {
    ESP_LOGI(tag, "Starting HTTPS Websocket Server");

    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
    conf.httpd.max_open_sockets = MAX_HTTP_CLIENTS;
    conf.httpd.global_user_ctx = nullptr;
    conf.httpd.open_fn = wssOpenFd;
    // conf.httpd.close_fn = wssCloseFd;

    // Ask key_cert_manager to attach cert pointers to config
    ESP_ERROR_CHECK(attachKeyCertBundleToConfig(&conf));

    ret = httpd_ssl_start(&server, &conf);
    if (ret != ESP_OK) {
      ESP_LOGE(tag, "Error starting httpd ssl server: %d", ret);
      return;
    }
    ESP_LOGI(tag, "Started httpd ssl server successfully");
  } else {
    ESP_LOGI(tag, "Starting HTTP Websocket Server (INSECURE)");

    httpd_config_t conf = HTTPD_DEFAULT_CONFIG();
    conf.max_open_sockets = MAX_HTTP_CLIENTS;
    conf.global_user_ctx = nullptr;
    conf.open_fn = wssOpenFd;

    ret = httpd_start(&server, &conf);

    if (ret != ESP_OK) {
      ESP_LOGE(tag, "Error starting httpd insecure server: %d", ret);
      return;
    }
    ESP_LOGI(tag, "Started httpd insecure server successfully");
  }

  ESP_LOGI(tag, "Registering URI Handlers");
  httpd_register_uri_handler(server, &kWsUriConf);
}

static esp_err_t stopWssServer() {
  ESP_LOGI(tag, "Stopping WebSocket Server");
  return httpd_ssl_stop(server);
}

// Called when wifi/ethernet disconnects
static void disconnectHandler(void *arg __unused,
                              esp_event_base_t event_base __unused,
                              int32_t event_id __unused,
                              void *event_data __unused) {
  if (server != nullptr) {
    esp_err_t ret = stopWssServer();
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
    startWssServer();
  } else {
    ESP_LOGE(tag, "connectHandler called but server address was not null!");
  }
}

// Note this pattern for safe ownership transfer into a C function!!!
static void wssServerSendMessages() {
  bool send_messages = true;

  while (send_messages) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    if (server == nullptr) {
      ESP_LOGD(tag, "SendMessages looping, server ref is nullptr");
      continue; // Server might not be created
    }
    ESP_LOGI(tag, "Sending messages to all http clients");

    size_t clients = MAX_HTTP_CLIENTS;
    std::array<int, MAX_HTTP_CLIENTS> client_fds{};

    if (httpd_get_client_list(server, &clients, client_fds.data()) == ESP_OK) {
      for (size_t i = 0; i < clients; i++) {
        int sock = client_fds.at(i);

        if (httpd_ws_get_fd_info(server, sock) == HTTPD_WS_CLIENT_WEBSOCKET) {
          ESP_LOGI(tag, "Websocket client: fd=%d -> sending async message",
                   sock);

          std::unique_ptr<AsyncRespArg> resp_arg =
              std::make_unique<AsyncRespArg>();

          resp_arg->hd = server;
          resp_arg->fd = sock;

          esp_err_t ret =
              httpd_queue_work(resp_arg->hd, sendHello, resp_arg.get());

          if (ret == ESP_OK) {
            // httpd will call delete on pointer.
            // Release here to avoid double free.
            resp_arg.release(); // NOLINT(bugprone-unused-return-value)
          } else {
            ESP_LOGE(tag, "httpd_queue_work failed");
            send_messages = false;
          }
        }
      }
    } else {
      ESP_LOGE(tag, "httpd_get_client_list failed!");
      return;
    }
  }
}

void wssServerTask(void *args __unused) {
  psa_crypto_init();

  ESP_LOGI(tag, "Registering connect/disconnect handlers");
  // Register server start/stop functions
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                             &connectHandler, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(
      ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, &disconnectHandler, nullptr));

  ESP_LOGI(tag, "Registered connect/disconnect handlers");

  while (true) {
    vTaskDelay(portMAX_DELAY);
  }

  // wssServerSendMessages();
}