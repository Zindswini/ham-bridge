#include "request_handlers.h"

// Standard Library
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>

// ESP-IDF
#include <esp_err.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <sys/_timeval.h>
#include <sys/time.h>

// External Components
#include "enums/status_code.pb.h"
#include "messages/core_envelope.pb.h"
#include "messages/hello.pb.h"
#include "messages/ping.pb.h"
#include "messages/status.pb.h"
#include "nanopb_helper.hpp"
#include "pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

// Local Includes
#include "config.h"
#include "metadata.hpp"

static const char *tag = "REQUEST_HANDLERS";

static std::array<uint8_t, 8192> proto_stream_buf{};
std::shared_mutex proto_stream_mutex;

esp_err_t dispatchProtobufRequest(std::vector<uint8_t> &buf, httpd_req_t &req) {
  esp_err_t ret = ESP_OK;
  // Expect all protobuf packets to be wrapped in an "Envelope" message
  Zindswini_ham_bridge_Envelope envelope;
  pb_istream_t stream = pb_istream_from_buffer(buf.data(), buf.size());
  if (!pb_decode(&stream, Zindswini_ham_bridge_Envelope_fields, &envelope)) {
    ESP_LOGE(tag, "Failed to decode incoming packet as protobuf Envelope");
    return ESP_ERR_INVALID_ARG;
  }
  // TODO(Zindswini): Return error messages over proto
  switch (envelope.which_kind) {
    case Zindswini_ham_bridge_Envelope_request_tag:
      switch (envelope.kind.request.which_method) {
        case Zindswini_ham_bridge_Request_hello_request_tag:
          ret = handleHelloRequest(envelope.kind.request, req);
          break;
        case Zindswini_ham_bridge_Request_ping_request_tag:
          ret = handlePingRequest(envelope.kind.request, req);
          break;
        case Zindswini_ham_bridge_Request_subscribe_request_tag:
          break;
        case Zindswini_ham_bridge_Request_get_state_request_tag:
          break;
        case Zindswini_ham_bridge_Request_set_state_request_tag:
          break;
        case Zindswini_ham_bridge_Request_ptt_request_tag:
          break;
        default:
          break;
      }
      break;

    case Zindswini_ham_bridge_Envelope_push_event_tag:
      switch (envelope.kind.push_event.which_payload) {
        case Zindswini_ham_bridge_PushEvent_audio_frame_tag:
          break;
        default:
          break;
      }
      break;

    case Zindswini_ham_bridge_Envelope_response_tag:
      ESP_LOGE(tag, "Server received protobuf envelope with type Response");
      break;

    default:
      ESP_LOGE(tag, "Server received protobuf envelope with unknown/missing oneof");
      break;
  }
  return ret;
};

esp_err_t encodeAndSendPayload(const Zindswini_ham_bridge_Envelope &response, httpd_req_t &req) {
  std::unique_lock lock(proto_stream_mutex);
  pb_ostream_t ostream = pb_ostream_from_buffer(proto_stream_buf.data(), proto_stream_buf.size());
  if (!pb_encode(&ostream, Zindswini_ham_bridge_Envelope_fields, &response)) {
    ESP_LOGE(tag, "Failed to Encode Protobuf With Error: %s", ostream.errmsg);
    return ESP_FAIL;
  }

  // Send response
  httpd_ws_frame_t ws_pkt = {
      .final = true,
      .fragmented = false,
      .type = HTTPD_WS_TYPE_BINARY,
      .payload = proto_stream_buf.data(),
      .len = ostream.bytes_written,
  };
  esp_err_t ret = httpd_ws_send_frame_async(req.handle, httpd_req_to_sockfd(&req), &ws_pkt);
  if (ret != ESP_OK) {
    ESP_LOGE(tag, "Failed to send response for handleHelloRequest");
    return ret;
  }
  return ESP_OK;
}

esp_err_t handleHelloRequest(Zindswini_ham_bridge_Request &payload, httpd_req_t &req) {
  assert(payload.which_method == Zindswini_ham_bridge_Request_hello_request_tag);

  // Build response payload
  const Zindswini_ham_bridge_Status status = {
      .code = Zindswini_ham_bridge_StatusCode_STATUS_CODE_OK,
      .message = npb::str("Hello!"),
  };

  const Zindswini_ham_bridge_HelloResponse hello_response = {
      .protocol_version = PROTOCOL_VERSION,
      .session_id = static_cast<uint32_t>(httpd_req_to_sockfd(&req)),
      .claimed = true,
      .has_server_info = true,
      .server_info = kServerInfo,
  };
  const Zindswini_ham_bridge_Envelope response = {
      .which_kind = Zindswini_ham_bridge_Envelope_response_tag,
      .kind = {.response = {
                   .request_id = payload.request_id,
                   .has_status = true,
                   .status = status,
                   .which_result = Zindswini_ham_bridge_Response_hello_response_tag,
                   .result = {.hello_response = hello_response},
               }}};

  // Send response
  esp_err_t ret = encodeAndSendPayload(response, req);
  if (ret != ESP_OK) {
    ESP_LOGE(tag, "Failed to send response for handleHelloRequest");
    return ret;
  }

  return ESP_OK;
}

esp_err_t handlePingRequest(Zindswini_ham_bridge_Request &payload, httpd_req_t &req) {
  assert(payload.which_method == Zindswini_ham_bridge_Request_ping_request_tag);

  ESP_LOGD(tag, "Got protobuf ping request with timestamp %d", payload.method.ping_request.client_time_us);

  // Build Response Payload
  timeval time{};
  gettimeofday(&time, nullptr);
  const uint64_t time_us = (time.tv_sec * 1'000'000) + time.tv_usec;
  const Zindswini_ham_bridge_PingResponse pingResponse = {
      .client_time_us = payload.method.ping_request.client_time_us,
      .server_time_us = time_us,
  };
  const Zindswini_ham_bridge_Envelope response = {
      .which_kind = Zindswini_ham_bridge_Envelope_response_tag,
      .kind =
          {
              .response =
                  {
                      .request_id = payload.request_id,
                      .has_status = true,
                      .status = {.code = Zindswini_ham_bridge_StatusCode_STATUS_CODE_OK, .message = npb::str("OK")},
                      .which_result = Zindswini_ham_bridge_Response_ping_response_tag,
                      .result = {.ping_response = pingResponse},
                  },
          },
  };

  // Send Response
  esp_err_t ret = encodeAndSendPayload(response, req);
  if (ret != ESP_OK) {
    ESP_LOGE(tag, "Failed to send response for handleHelloRequest");
    return ret;
  }

  return ESP_OK;
}