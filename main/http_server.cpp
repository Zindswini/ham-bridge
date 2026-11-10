#include "http_server.h"

// Standard Library
#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
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
#include "enums/audio_codec.pb.h"
#include "messages/core_envelope.pb.h"

// Local Includes
#include "config.h"
#include "i2s_handler.h"
#include "key_cert_manager.h"
#include "nanopb_helper.hpp"
#include "pb.h"
#include "pb_encode.h"
#include "request_handlers.h"

// Implementation Reference:
// https://github.com/espressif/esp-idf/blob/v6.0.1/examples/protocols/https_server/wss_server/main/wss_server_example.c

static const char *tag = "HTTPS_WSS_SERVER";
static httpd_handle_t server = nullptr;
static std::atomic_uint32_t audio_frame_sequence = 0;

struct EncodeSlot {
  std::array<uint8_t, static_cast<size_t>(1.2 * BUFFER_SLOT_BYTES)> data{};
  size_t len = 0;
  [[nodiscard]] std::span<const uint8_t> used() const { return {data.data(), len}; }
  SemaphoreHandle_t readers{};
};

struct BroadcastRespArg {
  httpd_handle_t hd;
  int fd;
  EncodeSlot *slot = nullptr;
};

static EncodeSlot slot_a, slot_b;
void initSlots() {
  slot_a.readers = xSemaphoreCreateCounting(MAX_HTTP_CLIENTS, 0);
  slot_b.readers = xSemaphoreCreateCounting(MAX_HTTP_CLIENTS, 0);
};

static EncodeSlot &wait_free_slot() {
  for (;;) {
    if (uxSemaphoreGetCount(slot_a.readers) == 0) {
      return slot_a;
    }
    if (uxSemaphoreGetCount(slot_b.readers) == 0) {
      return slot_b;
    }
    vTaskDelay(1);
  }
}

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
  if (incoming_ws_pkt.len == 0 || incoming_ws_pkt.len > MAX_INCOMING_WS_SIZE) {
    ESP_LOGE(tag,
             "httpd_ws_recv_frame returned invalid payload size (%d)! Max "
             "size: (%d)!",
             incoming_ws_pkt.len, MAX_INCOMING_WS_SIZE);
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

static esp_err_t encodeNextAudioChunk(EncodeSlot &target) {
  Chunk *chunk = nullptr;
  // TODO(Zindswini): Is there a reason *not* to wait here almost indefinitely?
  while (chunk == nullptr) {
    if (xQueueReceive(queue_filled, static_cast<void *>(&chunk), 0) != pdTRUE) {
      ESP_LOGW(tag, "No filled chunk in queue, having to wait on I2S!");
      vTaskDelay(pdMS_TO_TICKS(100));
      // return;
    }
  }

  timeval time{};
  gettimeofday(&time, nullptr);
  const uint64_t time_us = (time.tv_sec * 1'000'000) + time.tv_usec;

  // Have chunk full of audio data, now wrap in Audio Frame Push-Event
  const Zindswini_ham_bridge_AudioFrame audio_frame = {
      .stream_id = 1,
      .sequence = ++audio_frame_sequence,
      .timestamp_us = time_us,
      .codec = Zindswini_ham_bridge_AudioCodec_AUDIO_CODEC_ADPCM,
      .sample_rate_hz = I2S_SAMPLE_RATE,
      .channels = 2,
      .data = npb::bytes(chunk),
  };

  const Zindswini_ham_bridge_Envelope payload = {
      .which_kind = Zindswini_ham_bridge_Envelope_push_event_tag,
      .kind = {
          .push_event = {.which_payload = Zindswini_ham_bridge_PushEvent_audio_frame_tag,
                         .payload = {.audio_frame = audio_frame}},
      }};

  // Have protubuf object, now encode into at the given target
  pb_ostream_t ostream = pb_ostream_from_buffer(target.data.data(), target.data.size());
  if (!pb_encode(&ostream, Zindswini_ham_bridge_Envelope_fields, &payload)) {
    // Protobuf encode failed for some reason.
    // Put chunk back into used queue for re-sending
    ESP_LOGE(tag, "Failed to encode chunk with error: %s", ostream.errmsg);
    xQueueSend(queue_filled, static_cast<void *>(&chunk), portMAX_DELAY);
    return ESP_FAIL;
  } else {
    target.len = ostream.bytes_written;
    ESP_LOGI(tag, "Encoded chunk with addr %p, sequence number %d, raw size: %d, encoded size: %d",
             chunk->storage.data(), audio_frame_sequence.load(), chunk->len, ostream.bytes_written);
  }

  // Successful encode, put chunk back into free queue
  chunk->len = 0;
  if (xQueueSend(queue_free, static_cast<void *>(&chunk), 0) != pdTRUE) {
    ESP_LOGW(tag, "No free chunk left, I2S must be too slow!");
    xQueueSend(queue_free, static_cast<void *>(&chunk),
               portMAX_DELAY);  // So we don't lose access to a chunk
  } else {
    ESP_LOGD(tag, "Returned chunk with addr %p to free queue", chunk->storage.data());
  }
  return ESP_OK;
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

static void sendSlotToClient(void *arg) {
  // Take ownership of data at arg (cleaned up automatically)
  auto resp_arg = std::unique_ptr<BroadcastRespArg>(static_cast<BroadcastRespArg *>(arg));

  httpd_ws_frame_t ws_pkt = {
      .final = true,
      .fragmented = false,
      .type = HTTPD_WS_TYPE_BINARY,
      .payload = resp_arg->slot->data.data(),
      .len = resp_arg->slot->len,
  };
  // This is a blocking function. After it is successful we no longer need
  // this chunk's data. Don't be fooled by async in the name.
  esp_err_t ret = httpd_ws_send_frame_async(resp_arg->hd, resp_arg->fd, &ws_pkt);
  if (ret != ESP_OK) {
    ESP_LOGE(tag, "Failed to send response for handleHelloRequest");
  }
  xSemaphoreTake(resp_arg->slot->readers, 0);
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

    // Make sure we have an open slot before we check for clients.
    // This can take a while in case of slow connection to client
    EncodeSlot &target_slot = wait_free_slot();

    size_t clients = MAX_HTTP_CLIENTS;
    std::array<int, MAX_HTTP_CLIENTS> client_fds{};

    if (httpd_get_client_list(server, &clients, client_fds.data()) == ESP_OK) {
      if (clients == 0) {
        // Long delay and loop
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }

      // We have at least one client, get and encode a chunk
      if (encodeNextAudioChunk(target_slot) != ESP_OK) {
        ESP_LOGE(tag, "Failed to get and encode audio chunk");
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
      };
      // Have protobuf encoded payload in target slot!

      for (size_t i = 0; i < clients; i++) {
        int sock = client_fds.at(i);

        if (httpd_ws_get_fd_info(server, sock) == HTTPD_WS_CLIENT_WEBSOCKET) {
          ESP_LOGI(tag, "Websocket client: fd=%d -> sending async message", sock);

          auto resp_arg = std::make_unique<BroadcastRespArg>();
          resp_arg->hd = server;
          resp_arg->fd = sock;
          resp_arg->slot = &target_slot;

          xSemaphoreGive(resp_arg->slot->readers);
          esp_err_t ret = httpd_queue_work(resp_arg->hd, sendSlotToClient, resp_arg.get());

          if (ret == ESP_OK) {
            // httpd will call delete on pointer.
            // Release here to avoid double free.
            resp_arg.release();  // NOLINT(bugprone-unused-return-value)
            ESP_LOGD(tag, "Successfully queued audioPacket work to httpd");
          } else {
            ESP_LOGE(tag, "httpd_queue_work failed: Error %d", ret);
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
  initSlots();

  ESP_LOGI(tag, "Registering connect/disconnect handlers");
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &connectHandler, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, &disconnectHandler, nullptr));
  ESP_LOGI(tag, "Registered connect/disconnect handlers");

  wssServerSendMessages();

  while (true) {
    vTaskDelay(portMAX_DELAY);
  }
}