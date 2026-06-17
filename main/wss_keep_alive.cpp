/* Keep Alive engine for wss server example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.

    https://raw.githubusercontent.com/espressif/esp-idf/refs/tags/v6.0.1/examples/protocols/https_server/wss_server/main/keep_alive.c
*/

#include "wss_keep_alive.h"
#include "esp_timer.h"
#include <cstddef>
#include <cstdint>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <memory>
#include <vector>

typedef enum : uint8_t {
  kNoClient = 0,
  kClientFdAdd,
  kClientFdRemove,
  kClientUpdate,
  kClientActive,
  kStopTask,
} client_fd_action_type_t;

typedef struct {
  client_fd_action_type_t type;
  int fd;
  uint64_t last_seen;
} client_fd_action_t;

typedef struct wss_keep_alive_storage {
  size_t max_clients;
  wss_check_client_alive_cb_t check_client_alive_cb;
  wss_check_client_alive_cb_t client_not_alive_cb;
  size_t keep_alive_period_ms;
  size_t not_alive_after_ms;
  void *user_ctx;
  QueueHandle_t q;
  std::vector<client_fd_action_t> clients;
} wss_keep_alive_storage_t;

typedef struct wss_keep_alive_storage *wss_keep_alive_t;

static const char *tag = "wss_keep_alive";

static uint64_t tickGetMs(void) { return esp_timer_get_time() / 1000; }

// Goes over active clients to find out how long we could sleep before checking
// who's alive
static uint64_t getMaxDelay(wss_keep_alive_t keep_alive_handle) {
  uint64_t check_after_ms = 30000; // max delay, no need to check anyone
  for (size_t i = 0; i < keep_alive_handle->max_clients; ++i) {
    if (keep_alive_handle->clients[i].type == kClientActive) {
      uint64_t check_this_client_at = keep_alive_handle->clients[i].last_seen +
                                      keep_alive_handle->keep_alive_period_ms;
      if (check_this_client_at < check_after_ms + tickGetMs()) {
        if (tickGetMs() > check_this_client_at) {
          check_after_ms =
              1000; // min delay, some client(s) not responding already
        }
      }
    }
  }
  return check_after_ms;
}

static bool updateClient(wss_keep_alive_t keep_alive_handle, int sockfd,
                         uint64_t timestamp) {
  for (size_t i = 0; i < keep_alive_handle->max_clients; ++i) {
    if (keep_alive_handle->clients[i].type == kClientActive &&
        keep_alive_handle->clients[i].fd == sockfd) {
      keep_alive_handle->clients[i].last_seen = timestamp;
      return true;
    }
  }
  return false;
}

static bool removeClient(wss_keep_alive_t keep_alive_handle, int sockfd) {
  for (size_t i = 0; i < keep_alive_handle->max_clients; ++i) {
    if (keep_alive_handle->clients[i].type == kClientActive &&
        keep_alive_handle->clients[i].fd == sockfd) {
      keep_alive_handle->clients[i].type = kNoClient;
      keep_alive_handle->clients[i].fd = -1;
      return true;
    }
  }
  return false;
}
static bool addNewClient(wss_keep_alive_t keep_alive_handle, int sockfd) {
  for (size_t i = 0; i < keep_alive_handle->max_clients; ++i) {
    if (keep_alive_handle->clients[i].type == kNoClient) {
      keep_alive_handle->clients[i].type = kClientActive;
      keep_alive_handle->clients[i].fd = sockfd;
      keep_alive_handle->clients[i].last_seen = tickGetMs();
      return true; // success
    }
  }
  return false;
}

static void keepAliveTask(void *arg) {
  auto *keep_alive_storage = static_cast<wss_keep_alive_storage_t *>(arg);
  bool run_task = true;
  client_fd_action_t client_action;
  while (run_task) {
    if (xQueueReceive(keep_alive_storage->q, (void *)&client_action,
                      getMaxDelay(keep_alive_storage) / portTICK_PERIOD_MS) ==
        pdTRUE) {
      switch (client_action.type) {
      case kClientFdAdd:
        if (!addNewClient(keep_alive_storage, client_action.fd)) {
          ESP_LOGE(tag, "Cannot add new client");
        }
        break;
      case kClientFdRemove:
        if (!removeClient(keep_alive_storage, client_action.fd)) {
          ESP_LOGE(tag, "Cannot remove client fd:%d", client_action.fd);
        }
        break;
      case kClientUpdate:
        if (!updateClient(keep_alive_storage, client_action.fd,
                          client_action.last_seen)) {
          ESP_LOGE(tag, "Cannot find client fd:%d", client_action.fd);
        }
        break;
      case kStopTask:
        run_task = false;
        break;
      default:
        ESP_LOGE(tag, "Unexpected client action");
        break;
      }
    } else {
      // timeout: check if PING message needed
      for (size_t i = 0; i < keep_alive_storage->max_clients; ++i) {
        if (keep_alive_storage->clients[i].type == kClientActive) {
          if (keep_alive_storage->clients[i].last_seen +
                  keep_alive_storage->keep_alive_period_ms <=
              tickGetMs()) {
            ESP_LOGD(tag, "Haven't seen the client (fd=%d) for a while",
                     keep_alive_storage->clients[i].fd);
            if (keep_alive_storage->clients[i].last_seen +
                    keep_alive_storage->not_alive_after_ms <=
                tickGetMs()) {
              ESP_LOGE(tag, "Client (fd=%d) not alive!",
                       keep_alive_storage->clients[i].fd);
              keep_alive_storage->client_not_alive_cb(
                  keep_alive_storage, keep_alive_storage->clients[i].fd);
            } else {
              keep_alive_storage->check_client_alive_cb(
                  keep_alive_storage, keep_alive_storage->clients[i].fd);
            }
          }
        }
      }
    }
  }
  vQueueDelete(keep_alive_storage->q);
  free(keep_alive_storage);

  vTaskDelete(NULL);
}

wss_keep_alive_t wssKeepAliveStart(wss_keep_alive_config_t *config) {
  size_t queue_size = config->max_clients / 2;
  size_t client_list_size = config->max_clients + queue_size;
  wss_keep_alive_t keep_alive_storage =
      calloc(1, sizeof(wss_keep_alive_storage_t) +
                    client_list_size * sizeof(client_fd_action_t));
  if (keep_alive_storage == NULL) {
    return nullptr;
  }
  keep_alive_storage->check_client_alive_cb = config->check_client_alive_cb;
  keep_alive_storage->client_not_alive_cb = config->client_not_alive_cb;
  keep_alive_storage->max_clients = config->max_clients;
  keep_alive_storage->not_alive_after_ms = config->not_alive_after_ms;
  keep_alive_storage->keep_alive_period_ms = config->keep_alive_period_ms;
  keep_alive_storage->user_ctx = config->user_ctx;
  keep_alive_storage->q = xQueueCreate(queue_size, sizeof(client_fd_action_t));
  if (xTaskCreate(keepAliveTask, "keep_alive_task", config->task_stack_size,
                  keep_alive_storage, config->task_prio, NULL) != pdTRUE) {
    wssKeepAliveStop(keep_alive_storage);
    return nullptr;
  }
  return keep_alive_storage;
}

void wssKeepAliveStop(wss_keep_alive_t keep_alive_handle) {
  client_fd_action_t stop = {.type = kStopTask, .fd = -1, .last_seen = 0};
  xQueueSendToBack(keep_alive_handle->q, &stop, 0);
  // internal structs will be de-allocated in the task
}

esp_err_t wssKeepAliveAddClient(wss_keep_alive_t keep_alive_handle,
                                int file_desc) {
  client_fd_action_t client_fd_action = {
      .type = kClientFdAdd,
      .fd = file_desc,
      .last_seen = tickGetMs(),
  };
  if (xQueueSendToBack(keep_alive_handle->q, &client_fd_action, 0) == pdTRUE) {
    return ESP_OK;
  }
  return ESP_FAIL;
}

esp_err_t wssKeepAliveRemoveClient(wss_keep_alive_t keep_alive_handle,
                                   int file_desc) {
  client_fd_action_t client_fd_action = {
      .type = kClientFdRemove, .fd = file_desc, .last_seen = tickGetMs()};
  if (xQueueSendToBack(keep_alive_handle->q, &client_fd_action, 0) == pdTRUE) {
    return ESP_OK;
  }
  return ESP_FAIL;
}

esp_err_t wssKeepAliveClientIsActive(wss_keep_alive_t keep_alive_handle,
                                     int file_desc) {
  client_fd_action_t client_fd_action = {
      .type = kClientUpdate, .fd = file_desc, .last_seen = tickGetMs()};
  if (xQueueSendToBack(keep_alive_handle->q, &client_fd_action, 0) == pdTRUE) {
    return ESP_OK;
  }
  return ESP_FAIL;
}

void wssKeepAliveSetUserCtx(wss_keep_alive_t keep_alive_handle, void *ctx) {
  keep_alive_handle->user_ctx = ctx;
}

void *wssKeepAliveGetUserCtx(wss_keep_alive_t keep_alive_handle) {
  return keep_alive_handle->user_ctx;
}
