#include "ethernet_handler.h"

#include "esp_err.h"
#include "esp_eth_driver.h"
#include "esp_eth_netif_glue.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_netif_ip_addr.h"
#include "esp_netif_types.h"
#include "ethernet_init.h"
#include "freertos/idf_additions.h"
#include <cstdlib>
#include <cstring>
#include <format>
#include <sys/cdefs.h>
#include <vector>

static const char *tag = "ethernet_handler";

std::vector<esp_eth_handle_t> eth_handles;
static SemaphoreHandle_t ip_got_sem;
static esp_netif_t *eth_netif;

static void gotIpEventHandler(void *arg __unused,
                              esp_event_base_t event_base __unused,
                              int32_t event_id __unused,
                              void *event_data __unused) {
  xSemaphoreGive(ip_got_sem);
}

void setupEthernet(void) {
  ip_got_sem = xSemaphoreCreateBinary();
  if (ip_got_sem == nullptr) {
    ESP_LOGE(tag, "Failed to create semaphore");
    return;
  }

  // Get ethernet handles
  static esp_eth_handle_t *s_eth_handles = nullptr;
  static uint8_t s_eth_port_cnt = 0;
  ESP_ERROR_CHECK(ethernet_init_all(&s_eth_handles, &s_eth_port_cnt));

  // Copy into safe type
  eth_handles = std::vector<esp_eth_handle_t>(s_eth_port_cnt);
  memcpy(static_cast<void *>(eth_handles.data()),
         static_cast<const void *>(s_eth_handles),
         s_eth_port_cnt * sizeof(esp_eth_handle_t));
  free(s_eth_handles); // NOLINT(cppcoreguidelines-owning-memory,
                       // cppcoreguidelines-no-malloc)

  ESP_ERROR_CHECK(esp_netif_init());

  esp_netif_inherent_config_t esp_netif_config =
      ESP_NETIF_INHERENT_DEFAULT_ETH();
  esp_netif_config_t cfg_spi = {.base = &esp_netif_config,
                                .driver = nullptr,
                                .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH};

  // Iperf example loops here but I will only ever have one ethernet interface
  ESP_ERROR_CHECK(s_eth_port_cnt == 1 ? ESP_OK : ESP_FAIL);

  esp_netif_config.if_key = "ETH_1";
  esp_netif_config.if_desc = "eth1";
  // esp_netif_config.route_prio -= 5;

  eth_netif = esp_netif_new(&cfg_spi);
  // Attach Ethernet Driver to TCP/IP Stack
  ESP_ERROR_CHECK(
      esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handles.at(0))));

  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                             &gotIpEventHandler, nullptr));

  ESP_ERROR_CHECK(esp_eth_start(eth_handles.at(0)));

  // Delay returning until IP assigned
  // if (xSemaphoreTake(ip_got_sem, portMAX_DELAY) != pdTRUE) {
  //   ESP_LOGE(TAG, "Timeout waiting for ETH IP");
  // }
}

std::string getIpAddr() {
  std::string ip_str;
  esp_netif_ip_info_t ip_info;

  esp_netif_get_ip_info(eth_netif, &ip_info);
  ip_str = std::format(
      "{}.{}.{}.{}",
      IP2STR( // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-type-cstyle-cast)
          &ip_info.ip));
  return ip_str;
}
