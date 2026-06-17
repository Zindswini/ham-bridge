#include "http_server.h"

#include "esp_err.h"
#include "esp_eth_com.h"
#include "esp_event.h"
#include "esp_event_base.h"
#include "esp_http_server.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include "psa/crypto.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <esp_https_server.h>
#include <esp_log.h>
#include <esp_system.h>
#include <memory>
#include <nvs_flash.h>
#include <sys/cdefs.h>
#include <vector>

#include "config.h"
#include "esp_netif_types.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "psa/crypto_sizes.h"
#include "psa/crypto_struct.h"
#include "psa/crypto_types.h"
#include "psa/crypto_values.h"
#include "wss_keep_alive.h"

// Implementation Reference:
// https://github.com/espressif/esp-idf/blob/v6.0.1/examples/protocols/https_server/wss_server/main/wss_server_example.c

static const char *TAG = "HTTPS_WSS_SERVER";

struct AsyncRespArg {
  httpd_handle_t hd;
  int fd;
};

struct KeyCertBundle {
  std::vector<uint8_t> key;
  std::vector<uint8_t> cert;
};

static httpd_handle_t server = nullptr;
static KeyCertBundle bundle;

#define KEY_BITS 2048
#define NVS_CERT_NAMESPACE "tls_store"
#define NVS_TLS_CERT_KEY "cert_val"
#define NVS_TLS_KEY_KEY "key_val"

extern "C" esp_err_t generateNewKeyCertBundle() {
  // Use psa library to generate the key
  psa_key_attributes_t key_attr = PSA_KEY_ATTRIBUTES_INIT;
  // psa_set_key_type(&key_attr,
  //                  PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
  psa_set_key_type(&key_attr, PSA_KEY_TYPE_RSA_KEY_PAIR);
  psa_set_key_bits(&key_attr, KEY_BITS);
  psa_set_key_usage_flags(&key_attr, PSA_KEY_USAGE_SIGN_HASH |
                                         PSA_KEY_USAGE_VERIFY_HASH |
                                         PSA_KEY_USAGE_EXPORT);
  // psa_set_key_algorithm(&key_attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
  psa_set_key_algorithm(&key_attr, PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_SHA_256));

  psa_key_id_t key_id = 0;

  ESP_LOGI(TAG, "Generating key...");
  // Takes about 2m for 4096 bit RSA key
  psa_status_t psa_ret = psa_generate_key(&key_attr, &key_id);
  if (psa_ret != PSA_SUCCESS) {
    ESP_LOGE(TAG, "Failed to generate key! psa_ret=%d", psa_ret);
    return ESP_ERR_INVALID_STATE;
  }
  ESP_LOGI(TAG, "Generated key!");

  // Create mbedtls context around new key
  mbedtls_pk_context pk_ctx;
  mbedtls_pk_init(&pk_ctx);
  int ret = mbedtls_pk_wrap_psa(&pk_ctx, key_id);
  if (ret != 0) {
    ESP_LOGE(TAG, "Failed to create mbedtls context from psa key!");
    psa_destroy_key(key_id);
    mbedtls_pk_free(&pk_ctx);
    return ESP_ERR_INVALID_STATE;
  }

  // Export key as PEM
  std::vector<uint8_t> key_buf(
      PSA_EXPORT_KEY_OUTPUT_SIZE(PSA_KEY_TYPE_RSA_KEY_PAIR, KEY_BITS));

  // Should be safe size. mbedtls_x509write_crt_der will return
  // MBEDTLS_ERR_X509_BUFFER_TOO_SMALL (negative) if too small
  std::vector<uint8_t> crt_buf(4096);

  // Generate random value for serial field
  std::array<uint8_t, 16> serial;
  ret = psa_generate_random(serial.data(), serial.size());
  if (ret != 0) {
    ESP_LOGE(TAG,
             "Failed to generate random serial field for x509 certificate");
    return ESP_ERR_INVALID_STATE;
  }
  serial.at(0) = (serial.at(0) & 0x07F) | 0x01; // Ensure nonzero leading byte

  mbedtls_x509write_cert crt;
  mbedtls_x509write_crt_init(&crt);

  mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
  mbedtls_x509write_crt_set_subject_key(&crt, &pk_ctx);
  mbedtls_x509write_crt_set_issuer_key(&crt, &pk_ctx);
  mbedtls_x509write_crt_set_subject_name(&crt,
                                         "CN=ham-bridge,O=ham-bridge,C=US");
  mbedtls_x509write_crt_set_issuer_name(&crt,
                                        "CN=ham-bridge,O=ham-bridge,C=US");
  mbedtls_x509write_crt_set_serial_raw(&crt, serial.data(), serial.size());
  mbedtls_x509write_crt_set_validity(&crt, "20260101000000", "20270101000000");
  mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);

  mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);

  // Write Key DER
  int key_der_written =
      mbedtls_pk_write_key_der(&pk_ctx, key_buf.data(), key_buf.size());
  if (key_der_written <= 0) {
    ESP_LOGE(TAG, "Failed to write key DER to buffer!");
    psa_destroy_key(key_id);
    mbedtls_pk_free(&pk_ctx);
    mbedtls_x509write_crt_free(&crt);
    return ESP_ERR_INVALID_STATE;
  }
  key_buf.erase(key_buf.begin(), key_buf.end() - key_der_written);

  // Write Cert DER
  int crt_der_written =
      mbedtls_x509write_crt_der(&crt, crt_buf.data(), crt_buf.size());
  if (crt_der_written <= 0) {
    ESP_LOGE(TAG, "Failed to write cert DER to buffer!");
    psa_destroy_key(key_id);
    mbedtls_pk_free(&pk_ctx);
    mbedtls_x509write_crt_free(&crt);
    return ESP_ERR_INVALID_STATE;
  }
  crt_buf.erase(crt_buf.begin(), crt_buf.end() - crt_der_written);

  ESP_LOGI(TAG, "Exported key and cert to buffers");

  // Store key and cert to nvs

  // Free memory after exporting to buffer
  psa_destroy_key(key_id);
  mbedtls_pk_free(&pk_ctx);
  mbedtls_x509write_crt_free(&crt);

  // Try writing to nvs
  ESP_LOGI(TAG, "Writing key and cert to NVS");
  nvs_handle_t nvs_h = 0;

  esp_err_t err = nvs_open(NVS_CERT_NAMESPACE, NVS_READWRITE, &nvs_h);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open nvs namespace '%s'", NVS_CERT_NAMESPACE);
    return err;
  }

  ESP_LOGI(TAG, "Saving key to NVS");
  err = nvs_set_blob(nvs_h, NVS_TLS_KEY_KEY, key_buf.data(), key_buf.size());
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to write key to NVS");
    nvs_close(nvs_h);
    return err;
  }

  ESP_LOGI(TAG, "Saving cert to NVS");
  err = nvs_set_blob(nvs_h, NVS_TLS_CERT_KEY, crt_buf.data(), crt_buf.size());
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to write key to NVS");
    nvs_close(nvs_h);
    return err;
  }

  err = nvs_commit(nvs_h);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to commit NVS data");
    nvs_close(nvs_h);
    return err;
  }
  ESP_LOGI(TAG, "Wrote key and cert to NVS");

  nvs_close(nvs_h);

  ESP_LOGI(TAG, "Writing key and cert to global bundle object");
  bundle.key = key_buf;
  bundle.cert = crt_buf;
  ESP_LOGI(TAG, "Wrote key and cert to global bundle object");

  return ESP_OK;
}

extern "C" esp_err_t getKeyCertBundleFromNvs() {
  // Get a handle to NVS
  nvs_handle_t nvs_h = 0;
  esp_err_t err = nvs_open(NVS_CERT_NAMESPACE, NVS_READONLY, &nvs_h);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS namespace");
    return err;
  }

  // Pass nullptr to get_blob to get size of objects
  size_t nvs_key_size = 0;
  size_t nvs_crt_size = 0;
  err = nvs_get_blob(nvs_h, NVS_TLS_KEY_KEY, nullptr, &nvs_key_size);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get length of key in NVS");
    nvs_close(nvs_h);
    return err;
  }
  err = nvs_get_blob(nvs_h, NVS_TLS_CERT_KEY, nullptr, &nvs_crt_size);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get length of cert in NVS");
    nvs_close(nvs_h);
    return err;
  }

  // If sizes > 0, certs already written
  if (nvs_key_size > 0 && nvs_crt_size > 0) {
    bundle.key.resize(nvs_key_size);
    bundle.cert.resize(nvs_crt_size);
    err =
        nvs_get_blob(nvs_h, NVS_TLS_KEY_KEY, bundle.key.data(), &nvs_key_size);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to get read key from NVS to vector");
      nvs_close(nvs_h);
      return err;
    }
    err = nvs_get_blob(nvs_h, NVS_TLS_CERT_KEY, bundle.cert.data(),
                       &nvs_crt_size);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to get read cert from NVS to vector");
      nvs_close(nvs_h);
      return err;
    }
  } else {
    bundle.key.clear();
    bundle.cert.clear();
  }

  nvs_close(nvs_h);
  return ESP_OK;
}

static auto wsHandler(httpd_req_t *req) -> esp_err_t {
  if (req->method == HTTP_GET) {
    ESP_LOGI(TAG, "Handshake done, new websocket connection opened");
    return ESP_OK;
  }

  httpd_ws_frame_t ws_pkt;
  uint8_t *buf = nullptr;
  memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

  // Call receive w/ len 0 to extract just payload size
  esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame length with %d",
             ret);
    return ret;
  }

  ESP_LOGI(TAG, "Frame length is %d", ws_pkt.len);
  if (ws_pkt.len != 0) {
    // Allocate memory for the payload
    buf = static_cast<uint8_t *>(calloc(1, (ws_pkt.len + 1)));
    if (buf == nullptr) {
      ESP_LOGE(TAG, "Failed to calloc memory for buf with len %d",
               ws_pkt.len + 1);
      return ESP_ERR_NO_MEM;
    }

    ws_pkt.payload = buf;
    // Call receive w/ actual len, writes into allocated memory
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
      free(buf);
      return ret;
    }
  }

  // Now have payload written to ws_pkt

  // Handle PONG message
  if (ws_pkt.type == HTTPD_WS_TYPE_PONG) {
    ESP_LOGD(TAG, "Received PONG message");
    free(buf);
    return wss_keep_alive_client_is_active(
        static_cast<wss_keep_alive_t>(httpd_get_global_user_ctx(req->handle)),
        httpd_req_to_sockfd(req));
  }

  // If TEXT message, echo it back (for now) TODO:
  if (ws_pkt.type == HTTPD_WS_TYPE_TEXT || ws_pkt.type == HTTPD_WS_TYPE_PING ||
      ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
      ESP_LOGI(TAG, "Received packet with message %s", ws_pkt.payload);
    } else if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
      ESP_LOGI(TAG, "Got WS PING frame, replying w/ PONG");
      ws_pkt.type = HTTPD_WS_TYPE_PONG;
    } else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
      ws_pkt.len = 0;
      ws_pkt.payload = nullptr;
    }
    ret = httpd_ws_send_frame(req, &ws_pkt);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "httpd_ws_send_frame failed with %d", ret);
    }

    ESP_LOGI(TAG, "ws_handler: httpd_handle_t=%p, sockfd=%d, client_info=%d",
             req->handle, httpd_req_to_sockfd(req),
             httpd_ws_get_fd_info(req->handle, httpd_req_to_sockfd(req)));
    free(buf);
    return ret;
  }

  free(buf);
  return ESP_OK;
}

auto wssOpenFd(httpd_handle_t httpd_handle, int sockfd) -> esp_err_t {
  ESP_LOGI(TAG, "New client connected %d", sockfd);

  auto *keep_alive_inst =
      static_cast<wss_keep_alive_t>(httpd_get_global_user_ctx(httpd_handle));
  return wss_keep_alive_add_client(keep_alive_inst, sockfd);
}

void wssCloseFd(httpd_handle_t httpd_handle, int sockfd) {
  ESP_LOGI(TAG, "Client disconnected %d", sockfd);

  auto *keep_alive_inst =
      static_cast<wss_keep_alive_t>(httpd_get_global_user_ctx(httpd_handle));
  wss_keep_alive_remove_client(keep_alive_inst, sockfd);
  fclose((FILE *)sockfd);
}

static const httpd_uri_t kWsUriConf = {
    .uri = "/ws",
    .method = HTTP_GET,
    .handler = wsHandler,
    .user_ctx = NULL,
    .is_websocket = true,
    .handle_ws_control_frames = true,
    .supported_subprotocol = nullptr,
};

static void sendHello(void *arg) {
  static const char *data = "Hello client!";
  auto *resp_arg = static_cast<AsyncRespArg *>(arg);

  httpd_handle_t httpd_handle = resp_arg->hd;
  int file_desc = resp_arg->fd;
  httpd_ws_frame_t ws_pkt;
  memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

  ws_pkt.payload = (uint8_t *)data;
  ws_pkt.len = strlen(data);
  ws_pkt.type = HTTPD_WS_TYPE_TEXT;

  httpd_ws_send_frame_async(httpd_handle, file_desc, &ws_pkt);
  free(resp_arg);
}

static void sendPing(void *arg) {
  auto *resp_arg = static_cast<AsyncRespArg *>(arg);

  httpd_handle_t httpd_handle = resp_arg->hd;
  int file_desc = resp_arg->fd;
  httpd_ws_frame_t ws_pkt;
  memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

  ws_pkt.payload = nullptr;
  ws_pkt.len = 0;
  ws_pkt.type = HTTPD_WS_TYPE_PING;

  httpd_ws_send_frame_async(httpd_handle, file_desc, &ws_pkt);
  free(resp_arg);
}

auto clientNotAliveCallback(wss_keep_alive_t keep_alive_handle, int file_desc)
    -> bool {
  ESP_LOGE(TAG, "Client not alive, closing fd %d", file_desc);
  esp_err_t ret = httpd_sess_trigger_close(
      wss_keep_alive_get_user_ctx(keep_alive_handle), file_desc);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to close wss fd %d", file_desc);
    return false;
  }
  return true;
}

auto checkClientAliveCallback(wss_keep_alive_t keep_alive_handle, int file_desc)
    -> bool {
  ESP_LOGD(TAG, "Checking if client (fd=%d) is alive", file_desc);

  auto *resp_arg =
      static_cast<AsyncRespArg *>(malloc(sizeof(struct AsyncRespArg)));
  if (resp_arg == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate memory for wss healthcheck on fd %d",
             file_desc);
    assert(resp_arg != nullptr);
  }
  resp_arg->hd = wss_keep_alive_get_user_ctx(keep_alive_handle);
  resp_arg->fd = file_desc;

  return httpd_queue_work(resp_arg->hd, sendPing, resp_arg) == ESP_OK;
}

static void startWssEchoServer() {
  // Prepare keep alive engine
  wss_keep_alive_config_t keep_alive_config = KEEP_ALIVE_CONFIG_DEFAULT();
  keep_alive_config.max_clients = MAX_HTTPS_CLIENTS;
  keep_alive_config.client_not_alive_cb = clientNotAliveCallback;
  keep_alive_config.check_client_alive_cb = checkClientAliveCallback;

  wss_keep_alive_t keep_alive = wss_keep_alive_start(&keep_alive_config);

  ESP_LOGI(TAG, "Starting HTTPS Websocket Server");
  esp_err_t ret;

  httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
  conf.httpd.max_open_sockets = MAX_HTTPS_CLIENTS;
  conf.httpd.global_user_ctx = keep_alive;
  conf.httpd.open_fn = wssOpenFd;
  conf.httpd.close_fn = wssCloseFd;

  // Read key and cert from global bundle
  ret = getKeyCertBundleFromNvs();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get KeyCert bundle from NVS!");
    return;
  }
  if (bundle.key.size() == 0 || bundle.cert.size() == 0) {
    ESP_LOGI(TAG, "No cert bundle found, generating new!");
    ret = generateNewKeyCertBundle();
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Failed to generate new KeyCert bundle!");
      return;
    }
  } else {
    ESP_LOGI(TAG, "Read KeyCert bundle from NVS");
  }
  conf.servercert = bundle.cert.data();
  conf.servercert_len = bundle.cert.size();
  conf.prvtkey_pem = bundle.key.data();
  conf.prvtkey_len = bundle.key.size();

  ret = httpd_ssl_start(&server, &conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Error starting httpd ssl server: %d", ret);
    return;
  }
  ESP_LOGI(TAG, "Started httpd ssl server successfully");

  ESP_LOGI(TAG, "Registering URI Handlers");
  httpd_register_uri_handler(server, &kWsUriConf);
  wss_keep_alive_set_user_ctx(keep_alive, server);
}

static auto stopWssEchoServer() -> esp_err_t {
  wss_keep_alive_stop(
      static_cast<wss_keep_alive_t>(httpd_get_global_user_ctx(server)));
  return httpd_ssl_stop(server);
}

static void disconnectHandler(void *arg __unused,
                              esp_event_base_t event_base __unused,
                              int32_t event_id __unused,
                              void *event_data __unused) {
  if (server != nullptr) {
    esp_err_t ret = stopWssEchoServer();
    if (ret == ESP_OK) {
      server = nullptr;
      ESP_LOGI(TAG, "Successfully stopped wss echo server");
    } else {
      ESP_LOGE(TAG, "Failed to stop wss echo server with error %d", ret);
    }
  } else {
    ESP_LOGE(TAG, "disconnectHandler called but server ref is nullptr!");
  }
}

static void connectHandler(void *arg __unused,
                           esp_event_base_t event_base __unused,
                           int32_t event_id __unused,
                           void *event_data __unused) {
  if (server == nullptr) {
    ESP_LOGI(TAG, "Server setup handler called, starting server");
    startWssEchoServer();
  } else {
    ESP_LOGE(TAG, "connectHandler called but server address was not null!");
  }
}

static void wssServerSendMessages() {
  bool send_messages = true;

  while (send_messages) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (server == nullptr) {
      ESP_LOGD(TAG, "SendMessages looping, server ref is nullptr");
      continue; // Server might not be created
    }

    size_t clients = MAX_HTTPS_CLIENTS;
    std::array<int, MAX_HTTPS_CLIENTS> client_fds{};

    if (httpd_get_client_list(server, &clients, client_fds.data()) == ESP_OK) {
      for (size_t i = 0; i < clients; i++) {
        int sock = client_fds.at(i);
        if (httpd_ws_get_fd_info(server, sock) == HTTPD_WS_CLIENT_WEBSOCKET) {
          ESP_LOGI(TAG, "Active client: fd=%d -> sending async message", sock);
          auto resp_arg = std::shared_ptr<AsyncRespArg>();
          assert(resp_arg != nullptr);
          resp_arg->hd = server;
          resp_arg->fd = sock;
          if (httpd_queue_work(resp_arg->hd, sendHello, resp_arg.get()) !=
              ESP_OK) {
            ESP_LOGE(TAG, "httpd_queue_work failed");
            send_messages = false;
            break;
          }
        }
      }
    } else {
      ESP_LOGE(TAG, "httpd_get_client_list failed!");
      return;
    }
  }
}

extern "C" void wssServerTask() {
  psa_crypto_init();

  ESP_LOGI(TAG, "Registering connect/disconnect handlers");
  // Register server start/stop functions
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                             &connectHandler, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(
      ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, &disconnectHandler, nullptr));

  ESP_LOGI(TAG, "Registered connect/disconnect handlers");

  wssServerSendMessages();
}