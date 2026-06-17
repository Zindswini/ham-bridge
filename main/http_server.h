#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void wssServerTask();
esp_err_t generateNewKeyCertBundle();
esp_err_t getKeyCertBundleFromNvs();

#ifdef __cplusplus
}
#endif

#endif