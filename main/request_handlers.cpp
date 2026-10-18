#include "request_handlers.h"

// Standard Library
#include <memory>
#include <mutex>
#include <shared_mutex>

// ESP-IDF
#include <esp_log.h>

// External Components
#include "esp_http_server.h"
#include "messages/hello.pb.h"
#include "pb.h"
#include "pb_encode.h"

// Local Includes
#include "config.h"

static const char *tag = "REQUEST_HANDLERS";

static std::array<uint8_t, 8192> proto_stream_buf{};
std::shared_mutex proto_stream_mutex;

static void dispatchProtobufRequest();

static void handleWsPingRequest(void *arg) {
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

static void sendHello(void *arg) {
  std::unique_lock lock(proto_stream_mutex);
  pb_ostream_t stream =
      pb_ostream_from_buffer(proto_stream_buf.data(), proto_stream_buf.size());

  if (!pb_encode(&stream, Zindswini_ham_bridge_ServerInfo_fields,
                 &kServerInfo)) {
    ESP_LOGE(tag, "Failed to encode protobuf hello message");
    return;
  }

  // Take ownership of data at arg (cleaned up automatically)
  auto resp_arg =
      std::unique_ptr<AsyncRespArg>(static_cast<AsyncRespArg *>(arg));

  httpd_ws_frame_t ws_pkt = {
      .final = true,
      .fragmented = false,
      .type = HTTPD_WS_TYPE_BINARY,
      .payload = proto_stream_buf.data(),
      .len = stream.bytes_written,
  };

  esp_err_t ret =
      httpd_ws_send_frame_async(resp_arg->hd, resp_arg->fd, &ws_pkt);
  if (ret != ESP_OK) {
    ESP_LOGE(tag, "Failed to send websocket frame! ret = %d", ret);
  }
}
