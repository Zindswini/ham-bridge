#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "esp_http_server.h"

struct AsyncRespArg {
  httpd_handle_t hd;
  int fd;
};

#ifdef __cplusplus
extern "C" {
#endif

void wssServerTask(void *args);
bool checkServerUp();

#ifdef __cplusplus
}
#endif

#endif