#pragma once

#include "esp_http_server.h"

// This tells us what server to use and what client is the request for
struct AsyncRespArg {
  httpd_handle_t hd;
  int fd;
};

static void dispatchProtobufRequest();

static void handleWsPingRequest(void *arg);

static void handleHelloRequest(void *arg);
static void handleSubscribeRequest(void *arg);
static void handleGetStateRequest(void *arg);
static void handleSetStateRequest(void *arg);
static void handlePttRequest(void *arg);