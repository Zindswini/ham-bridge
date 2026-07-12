#ifndef KEY_CERT_MANAGER_H
#define KEY_CERT_MANAGER_H

#include "esp_err.h"
#include "esp_https_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t generateNewKeyCertBundle();
esp_err_t initKeyCertBundleFromNvs();
esp_err_t attachKeyCertBundleToConfig(httpd_ssl_config_t *conf);

#ifdef __cplusplus
}
#endif

#endif // KEY_CERT_MANAGER_H