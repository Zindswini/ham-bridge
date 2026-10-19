#pragma once

#include <cstdint>
#include <vector>

#include "esp_err.h"
#include "esp_http_server.h"
#include "messages/core_envelope.pb.h"

// This tells us what server to use and what client is the request for
struct AsyncRespArg {
  httpd_handle_t hd;
  int fd;
};

esp_err_t dispatchProtobufRequest(std::vector<uint8_t> &buf, httpd_req_t &req);

// static esp_err_t handleWsPingRequest(void *arg);

esp_err_t handleHelloRequest(Zindswini_ham_bridge_Request &payload, httpd_req_t &req);
esp_err_t handlePingRequest(Zindswini_ham_bridge_Request &payload, httpd_req_t &req);
// static esp_err_t handleSubscribeRequest(void *arg, httpd_req_t *req);
// static esp_err_t handleGetStateRequest(void *arg, httpd_req_t *req);
// static esp_err_t handleSetStateRequest(void *arg, httpd_req_t *req);
// static esp_err_t handlePttRequest(void *arg, httpd_req_t *req);

// static esp_err_t handleAudioFramePush(void *arg);