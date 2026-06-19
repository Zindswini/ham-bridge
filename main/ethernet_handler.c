#include "ethernet_handler.h"

#include "config.h"
#include "esp_err.h"
#include "esp_eth.h"
#include "esp_eth_driver.h"
#include "esp_eth_netif_glue.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_netif_ip_addr.h"
#include "esp_netif_types.h"
#include "ethernet_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "portmacro.h"

static const char *TAG = "ethernet_handler";

static esp_eth_handle_t *s_eth_handles = nullptr;
static uint8_t s_eth_port_cnt = 0;
static SemaphoreHandle_t ip_got_sem;
static esp_netif_t *eth_netif;

static void gotIpEventHandler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data) {
  xSemaphoreGive(ip_got_sem);
}

void setupEthernet(void) {
  ip_got_sem = xSemaphoreCreateBinary();
  if (ip_got_sem == nullptr) {
    ESP_LOGE(TAG, "Failed to create semaphore");
    return;
  }

  ESP_ERROR_CHECK(ethernet_init_all(&s_eth_handles, &s_eth_port_cnt));
  ESP_ERROR_CHECK(esp_netif_init());

  esp_netif_inherent_config_t esp_netif_config =
      ESP_NETIF_INHERENT_DEFAULT_ETH();
  esp_netif_config_t cfg_spi = {.base = &esp_netif_config,
                                .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH};

  // Iperf example loops here but I will only ever have one ethernet interface
  ESP_ERROR_CHECK(s_eth_port_cnt == 1 ? ESP_OK : ESP_FAIL);

  esp_netif_config.if_key = "ETH_1";
  esp_netif_config.if_desc = "eth1";
  // esp_netif_config.route_prio -= 5;

  eth_netif = esp_netif_new(&cfg_spi);
  // Attach Ethernet Driver to TCP/IP Stack
  ESP_ERROR_CHECK(
      esp_netif_attach(eth_netif, esp_eth_new_netif_glue(s_eth_handles[0])));

  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                             &gotIpEventHandler, nullptr));

  ESP_ERROR_CHECK(esp_eth_start(s_eth_handles[0]));

  // Delay returning until IP assigned
  // if (xSemaphoreTake(ip_got_sem, portMAX_DELAY) != pdTRUE) {
  //   ESP_LOGE(TAG, "Timeout waiting for ETH IP");
  // }
}

void *getIpAddr(void) {
  static char ip_str[16]; // "255.255.255.255\0"
  esp_netif_ip_info_t ip_info;
  esp_netif_get_ip_info(eth_netif, &ip_info);
  snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
  return ip_str;
}
