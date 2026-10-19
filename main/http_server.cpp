#include "http_server.h"

// Standard Library
#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>

// ESP-IDF
#include <esp_err.h>
#include <esp_eth_com.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_https_server.h>
#include <esp_log.h>
#include <esp_netif_types.h>
#include <esp_system.h>
#include <freertos/idf_additions.h>
#include <freertos/projdefs.h>
#include <portmacro.h>

// External Components
#include "enums/features.pb.h"
#include "enums/radio_models.pb.h"
#include "messages/hello.pb.h"
#include "pb.h"
#include "pb_encode.h"

// Local Includes
#include "config.h"
#include "i2s_handler.h"
#include "key_cert_manager.h"
#include "nanopb_helper.hpp"
#include "request_handlers.h"

// Implementation Reference:
// https://github.com/espressif/esp-idf/blob/v6.0.1/examples/protocols/https_server/wss_server/main/wss_server_example.c

static const char *tag = "HTTPS_WSS_SERVER";
static httpd_handle_t server = nullptr;

bool checkServerUp() { return server != nullptr; }
bool checkConnectionActive() {
  size_t clients = MAX_HTTP_CLIENTS;
  std::array<int, MAX_HTTP_CLIENTS> client_fds{};

  if (httpd_get_client_list(server, &clients, client_fds.data()) == ESP_OK) {
    return clients > 0;
  }
  return false;
}

// Handle an incoming websocket payload
static esp_err_t wsHandler(httpd_req_t *req) {
  // Get request indicates client is initiating a connection
  if (req->method == HTTP_GET) {
    ESP_LOGI(tag, "Handshake done, new websocket connection opened");
    return ESP_OK;
  }

  ESP_LOGV(tag, "ws_handler: httpd_handle_t=%p, sockfd=%d, client_info=%d", req->handle, httpd_req_to_sockfd(req),
           httpd_ws_get_fd_info(req->handle, httpd_req_to_sockfd(req)));

  // If not GET request, proceed to read the payload
  httpd_ws_frame_t incoming_ws_pkt = {
      .final = true,
      .fragmented = false,  // final must be true
      .type = HTTPD_WS_TYPE_CONTINUE,
      .payload = nullptr,
      .len = 0,
  };
  std::vector<uint8_t> buf;

  // Call receive w/ len 0 to extract just payload size
  esp_err_t ret = httpd_ws_recv_frame(req, &incoming_ws_pkt, 0);
  if (ret != ESP_OK) {
    ESP_LOGE(tag, "httpd_ws_recv_frame failed to get frame length with %d", ret);
    return ret;
  }

  // Check/validate payload size
  if (incoming_ws_pkt.len == 0 || incoming_ws_pkt.len > MAX_WS_PAYLOAD_SIZE) {
    ESP_LOGE(tag,
             "httpd_ws_recv_frame returned invalid payload size (%d)! Max "
             "size: (%d)!",
             incoming_ws_pkt.len, MAX_WS_PAYLOAD_SIZE);
    return ESP_ERR_INVALID_ARG;
  }

  // Allocate memory for the payload
  // TODO(Zindswini): Should this be a static allocation?
  buf.resize(incoming_ws_pkt.len);
  incoming_ws_pkt.payload = buf.data();

  // Actually recieve the request payload
  ret = httpd_ws_recv_frame(req, &incoming_ws_pkt, incoming_ws_pkt.len);
  if (ret != ESP_OK) {
    ESP_LOGE(tag, "httpd_ws_recv_frame failed with %d", ret);
    return ret;
  }
  // Resize again in case actual payload size was smaller than anticipated
  // TODO(Zindswini): Fully validate if this is needed
  buf.resize(incoming_ws_pkt.len);

  // Have metadata and request, dispatch on type of websocket packet
  switch (incoming_ws_pkt.type) {
    case HTTPD_WS_TYPE_PONG:
      ESP_LOGD(tag, "Received PONG message");
      // TODO(Zindswini): Reset connection health/timeout for this FD
      break;

    case HTTPD_WS_TYPE_TEXT:
      ESP_LOGW(tag, "Got unexpected TEXT type websocket message");
      break;

    case HTTPD_WS_TYPE_BINARY:
      // Treat as protobuf payload
      dispatchProtobufRequest(buf, *req);
      break;

    // Ping+close should be handled by http server. Streaming / fragmented
    // payloads aren't implemented
    case HTTPD_WS_TYPE_PING:
    case HTTPD_WS_TYPE_CLOSE:
    case HTTPD_WS_TYPE_CONTINUE:
    default:
      ESP_LOGE(tag, "Got unexpected websocket message type: %d, ignoring.", incoming_ws_pkt.type);
      break;
  }
  if (ret != ESP_OK) {
    ESP_LOGE(tag, "httpd_ws_send_frame failed with %d", ret);
  }
  return ret;
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

static void sendAudioPacket(void *arg) {
  // Lock static buffer
  // std::unique_lock lock(proto_stream_mutex);

  // pb_ostream_t stream = pb_ostream_from_buffer(proto_stream_buf.data(),
  // proto_stream_buf.size());

  // TODO(Zindswini): Create Protobuf metadata and object

  // Take ownership of data at arg (cleaned up automatically)
  auto resp_arg = std::unique_ptr<AsyncRespArg>(static_cast<AsyncRespArg *>(arg));

  Chunk *chunk = nullptr;
  // TODO(Zindswini): Is there a reason *not* to wait here almost indefinitely?
  if (xQueueReceive(queue_filled, static_cast<void *>(&chunk), 0) != pdTRUE) {
    ESP_LOGW(tag, "No filled chunk in queue, having to wait on I2S!");
    vTaskDelay(pdMS_TO_TICKS(100));
    return;
  }

  // Have audio data, now need to format into websocket payload
  httpd_ws_frame_t ws_pkt = {
      .final = true,
      .fragmented = false,
      .type = HTTPD_WS_TYPE_BINARY,
      .payload = chunk->used().data(),
      .len = chunk->used().size_bytes(),
  };

  // This is a blocking function. After it is successful we no longer need
  // this chunk's data. Don't be fooled by async in the name.
  esp_err_t ret = httpd_ws_send_frame_async(resp_arg->hd, resp_arg->fd, &ws_pkt);

  if (ret != ESP_OK) {
    // Put chunk back into used queue for re-sending
    ESP_LOGE(tag, "Failed to send websocket frame! ret = %d", ret);
    xQueueSend(queue_filled, static_cast<void *>(&chunk), portMAX_DELAY);
  } else {
    ESP_LOGI(tag, "Sent WSS Message of chunk with addr %p and size %d", chunk->storage.data(), chunk->len);
  }

  // Successful send, put into free queue
  chunk->len = 0;
  if (xQueueSend(queue_free, static_cast<void *>(&chunk), 0) != pdTRUE) {
    ESP_LOGW(tag, "No free chunk left, I2S must be too slow!");
    xQueueSend(queue_free, static_cast<void *>(&chunk),
               portMAX_DELAY);  // So we don't lose access to a chunk
  } else {
    ESP_LOGD(tag, "Returned chunk with addr %p to free queue", chunk->storage.data());
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
static void disconnectHandler(void *arg __unused, esp_event_base_t event_base __unused, int32_t event_id __unused,
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

static void connectHandler(void *arg __unused, esp_event_base_t event_base __unused, int32_t event_id __unused,
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
    if (server == nullptr) {
      ESP_LOGV(tag, "SendMessages looping, server ref is nullptr");
      // Prevent tight loop
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    size_t clients = MAX_HTTP_CLIENTS;
    std::array<int, MAX_HTTP_CLIENTS> client_fds{};

    if (httpd_get_client_list(server, &clients, client_fds.data()) == ESP_OK) {
      if (clients == 0) {
        // Long delay and loop
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }

      for (size_t i = 0; i < clients; i++) {
        int sock = client_fds.at(i);

        if (httpd_ws_get_fd_info(server, sock) == HTTPD_WS_CLIENT_WEBSOCKET) {
          ESP_LOGI(tag, "Websocket client: fd=%d -> sending async message", sock);

          auto resp_arg = std::make_unique<AsyncRespArg>();
          resp_arg->hd = server;
          resp_arg->fd = sock;

          esp_err_t ret = httpd_queue_work(resp_arg->hd, sendAudioPacket, resp_arg.get());

          // esp_err_t ret = httpd_queue_work(resp_arg->hd, handleHelloRequest, resp_arg.get());

          if (ret == ESP_OK) {
            // httpd will call delete on pointer.
            // Release here to avoid double free.
            resp_arg.release();  // NOLINT(bugprone-unused-return-value)
            ESP_LOGD(tag, "Successfully queued audioPacket work to httpd");
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

    // Prevent tight loop
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void wssServerTask(void *args __unused) {
  psa_crypto_init();

  ESP_LOGI(tag, "Registering connect/disconnect handlers");
  // Register server start/stop functions
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &connectHandler, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, &disconnectHandler, nullptr));

  ESP_LOGI(tag, "Registered connect/disconnect handlers");

  // wssServerSendMessages();

  while (true) {
    vTaskDelay(portMAX_DELAY);
  }
}