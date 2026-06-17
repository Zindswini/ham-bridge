#include "key_cert_manager.h"

#include <array>
#include <esp_http_server.h>
#include <esp_log.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509.h>
#include <mbedtls/x509_crt.h>
#include <nvs.h>
#include <psa/crypto_sizes.h>
#include <psa/crypto_struct.h>
#include <psa/crypto_types.h>
#include <psa/crypto_values.h>
#include <vector>

#include "config.h"
#include "esp_err.h"

static const char *tag = "KEY_CERT_MANAGER";

struct KeyCertBundle {
  std::vector<uint8_t> key;
  std::vector<uint8_t> cert;
};

static KeyCertBundle bundle;

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

  ESP_LOGI(tag, "Generating key...");
  // Takes about 2m for 4096 bit RSA key
  psa_status_t psa_ret = psa_generate_key(&key_attr, &key_id);
  if (psa_ret != PSA_SUCCESS) {
    ESP_LOGE(tag, "Failed to generate key! psa_ret=%d", psa_ret);
    return ESP_ERR_INVALID_STATE;
  }
  ESP_LOGI(tag, "Generated key!");

  // Create mbedtls context around new key
  mbedtls_pk_context pk_ctx;
  mbedtls_pk_init(&pk_ctx);
  int ret = mbedtls_pk_wrap_psa(&pk_ctx, key_id);
  if (ret != 0) {
    ESP_LOGE(tag, "Failed to create mbedtls context from psa key!");
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
  std::array<uint8_t, 16> serial{};
  ret = psa_generate_random(serial.data(), serial.size());
  if (ret != 0) {
    ESP_LOGE(tag,
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
    ESP_LOGE(tag, "Failed to write key DER to buffer!");
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
    ESP_LOGE(tag, "Failed to write cert DER to buffer!");
    psa_destroy_key(key_id);
    mbedtls_pk_free(&pk_ctx);
    mbedtls_x509write_crt_free(&crt);
    return ESP_ERR_INVALID_STATE;
  }
  crt_buf.erase(crt_buf.begin(), crt_buf.end() - crt_der_written);

  ESP_LOGI(tag, "Exported key and cert to buffers");

  // Store key and cert to nvs

  // Free memory after exporting to buffer
  psa_destroy_key(key_id);
  mbedtls_pk_free(&pk_ctx);
  mbedtls_x509write_crt_free(&crt);

  // Try writing to nvs
  ESP_LOGI(tag, "Writing key and cert to NVS");
  nvs_handle_t nvs_h = 0;

  esp_err_t err = nvs_open(NVS_CERT_NAMESPACE, NVS_READWRITE, &nvs_h);
  if (err != ESP_OK) {
    ESP_LOGE(tag, "Failed to open nvs namespace '%s'", NVS_CERT_NAMESPACE);
    return err;
  }

  ESP_LOGI(tag, "Saving key to NVS");
  err = nvs_set_blob(nvs_h, NVS_TLS_KEY_KEY, key_buf.data(), key_buf.size());
  if (err != ESP_OK) {
    ESP_LOGE(tag, "Failed to write key to NVS");
    nvs_close(nvs_h);
    return err;
  }

  ESP_LOGI(tag, "Saving cert to NVS");
  err = nvs_set_blob(nvs_h, NVS_TLS_CERT_KEY, crt_buf.data(), crt_buf.size());
  if (err != ESP_OK) {
    ESP_LOGE(tag, "Failed to write key to NVS");
    nvs_close(nvs_h);
    return err;
  }

  err = nvs_commit(nvs_h);
  if (err != ESP_OK) {
    ESP_LOGE(tag, "Failed to commit NVS data");
    nvs_close(nvs_h);
    return err;
  }
  ESP_LOGI(tag, "Wrote key and cert to NVS");

  nvs_close(nvs_h);

  ESP_LOGI(tag, "Writing key and cert to global bundle object");
  bundle.key = key_buf;
  bundle.cert = crt_buf;
  ESP_LOGI(tag, "Wrote key and cert to global bundle object");

  return ESP_OK;
}

extern "C" esp_err_t initKeyCertBundleFromNvs() {
  ESP_LOGI(tag, "Initializing KeyCert Bundle from NVS");
  // Get a handle to NVS
  nvs_handle_t nvs_h = 0;
  esp_err_t err = nvs_open(NVS_CERT_NAMESPACE, NVS_READONLY, &nvs_h);
  if (err != ESP_OK) {
    ESP_LOGE(tag, "Failed to open NVS namespace");
    return err;
  }

  // Pass nullptr to get_blob to get size of objects
  size_t nvs_key_size = 0;
  size_t nvs_crt_size = 0;
  err = nvs_get_blob(nvs_h, NVS_TLS_KEY_KEY, nullptr, &nvs_key_size);
  if (err != ESP_OK) {
    ESP_LOGE(tag, "Failed to get length of key in NVS");
    nvs_close(nvs_h);
    return err;
  }
  err = nvs_get_blob(nvs_h, NVS_TLS_CERT_KEY, nullptr, &nvs_crt_size);
  if (err != ESP_OK) {
    ESP_LOGE(tag, "Failed to get length of cert in NVS");
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
      ESP_LOGE(tag, "Failed to get read key from NVS to vector");
      nvs_close(nvs_h);
      return err;
    }
    err = nvs_get_blob(nvs_h, NVS_TLS_CERT_KEY, bundle.cert.data(),
                       &nvs_crt_size);
    if (err != ESP_OK) {
      ESP_LOGE(tag, "Failed to get read cert from NVS to vector");
      nvs_close(nvs_h);
      return err;
    }
  }
  // Size is zero, certs don't exist yet
  else {
    ESP_LOGI(tag, "KeyCert Bundle not found in NVS! Creating...");
    bundle.key.clear();
    bundle.cert.clear();
    return generateNewKeyCertBundle();
  }

  nvs_close(nvs_h);
  return ESP_OK;
}

extern "C" esp_err_t attachKeyCertBundleToConfig(httpd_ssl_config_t *conf) {
  esp_err_t ret = ESP_OK;

  // Read key and cert from global bundle
  if (bundle.key.size() == 0 || bundle.cert.size() == 0) {
    ESP_LOGI(tag, "No cert bundle found, generating new!");
    ret = generateNewKeyCertBundle();
    if (ret != ESP_OK) {
      ESP_LOGE(tag, "Failed to generate new KeyCert bundle!");
      return ret;
    }
  } else {
    ESP_LOGI(tag, "Read KeyCert bundle from NVS");
  }

  // TODO(Zindswini): UNSAFE!!!
  conf->servercert = bundle.cert.data();
  conf->servercert_len = bundle.cert.size();
  conf->prvtkey_pem = bundle.key.data();
  conf->prvtkey_len = bundle.key.size();

  return ret;
}