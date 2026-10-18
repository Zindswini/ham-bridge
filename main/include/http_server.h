#pragma once

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

void wssServerTask(void *args);
bool checkServerUp();
bool checkConnectionActive();

#ifdef __cplusplus
}
#endif