/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp_bsp_devkit.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "soc/gpio_num.h"
#include "support/CodeUtils.h"
#include <bsp/esp-bsp.h>
#include <esp_log.h>

#include <app_priv.h>
#include <button_gpio.h>
#include <esp_matter.h>
#include <iot_button.h>

using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "app_driver";

// State array tracking OnOff status locally for TOTAL_PLUG_COUNT + 1 plugs
static bool plug_states[TOTAL_PLUG_COUNT + 1] = {false};

// UART used for serial commands - using UART1 to separate from console logs
#define UART_PORT UART_NUM_1

#ifndef CONFIG_SERIAL_UART_TX_PIN
#define CONFIG_SERIAL_UART_TX_PIN 4
#endif

#ifndef CONFIG_SERIAL_UART_RX_PIN
#define CONFIG_SERIAL_UART_RX_PIN 5
#endif

#define UART_TX_PIN CONFIG_SERIAL_UART_TX_PIN
#define UART_RX_PIN CONFIG_SERIAL_UART_RX_PIN

esp_err_t app_driver_serial_init(void) {
  uart_config_t uart_config = {
      .baud_rate = 9600,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };

  // Install driver for UART1
  esp_err_t err = uart_driver_install(UART_PORT, 256 * 2, 0, 0, NULL, 0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "Failed UART driver install, err: %d", err);
    return err;
  }

  err = uart_param_config(UART_PORT, &uart_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed UART param config, err: %d", err);
    return err;
  }

  // Assign the pins configured in Kconfig
  err = uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed UART pin mapping, err: %d", err);
    return err;
  }

  ESP_LOGI(TAG,
           "Serial commands UART%d initialized at 9600 baud (TX:%d, RX:%d)",
           UART_PORT, UART_TX_PIN, UART_RX_PIN);
  return ESP_OK;
}

// Improved RX task with buffered reader ending on CR (0x0d) or LF (0x0a)
static void serial_rx_task(void *arg) {
  uint8_t rx_byte;
  char rx_buffer[64];
  int rx_index = 0;
  ESP_LOGI(TAG, "Serial RX Task started on UART%d", UART_PORT);

  while (1) {
    // Read 1 byte at a time to parse terminators immediately
    int len = uart_read_bytes(UART_PORT, &rx_byte, 1, portMAX_DELAY);
    if (len > 0) {
      // If we see CR (0x0d) or LF (0x0a), we treat it as end of command
      if (rx_byte == '\r' || rx_byte == '\n' || rx_byte == 0x0d) {
        if (rx_index > 0) {
          rx_buffer[rx_index] = 0; // Null terminate
          
          ESP_LOGI(TAG, "Serial RX Complete Command (%d bytes): [%s]", rx_index, rx_buffer);

          char *p = NULL;
          bool new_state = false;
          bool found = false;
          bool is_monostable_cmd = false;

          if ((p = strstr(rx_buffer, "on,")) != NULL) {
            new_state = true;
            found = true;
            is_monostable_cmd = false;
          } else if ((p = strstr(rx_buffer, "of,")) != NULL) {
            new_state = false;
            found = true;
            is_monostable_cmd = false;
          } else if ((p = strstr(rx_buffer, "za,")) != NULL) {
            new_state = true;
            found = true;
            is_monostable_cmd = true;
          } else if ((p = strstr(rx_buffer, "zs,")) != NULL) {
            new_state = false;
            found = true;
            is_monostable_cmd = true;
          }

          if (found) {
            int raw_idx = atoi(p + 3);
            int idx = is_monostable_cmd ? (raw_idx + NUM_BISTABLE_PLUGS) : raw_idx;

            ESP_LOGI(TAG, "Serial RX Task: Parsed Raw Index %d, Final Index %d, New State: %s", 
                     raw_idx, idx, new_state ? "ON" : "OFF");

            if (idx >= 1 && idx <= TOTAL_PLUG_COUNT) {
              uint16_t ep_id = plugin_unit_list[idx].endpoint_id;
              if (ep_id != 0xFFFF) {
                if (plug_states[idx] != new_state) {
                  plug_states[idx] = new_state;
                  esp_matter_attr_val_t val = esp_matter_bool(new_state);
                  ESP_LOGI(TAG,
                           "Serial RX Task: Updating Attribute Endpoint %d to %s",
                           ep_id, new_state ? "ON" : "OFF");

                  esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
                  esp_err_t err = attribute::update(
                      ep_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &val);
                  if (err != ESP_OK) {
                    ESP_LOGE(TAG,
                             "Failed to update Matter attribute for EP %d, err: %d",
                             ep_id, err);
                  }
                } else {
                  ESP_LOGI(TAG,
                           "Serial RX Task: Index %d already in state %s, skipping",
                           idx, new_state ? "ON" : "OFF");
                }
              } else {
                ESP_LOGW(TAG,
                         "Serial RX Task: Index %d has no endpoint ID assigned",
                         idx);
              }
            } else {
              ESP_LOGW(TAG, "Serial RX Task: Invalid index %d", idx);
            }
          }
          rx_index = 0; // Reset buffer index after processing
        }
      } else {
        // Collect characters into buffer (prevent overflow)
        if (rx_index < sizeof(rx_buffer) - 1) {
          rx_buffer[rx_index++] = (char)rx_byte;
        } else {
          // Buffer full, reset it to prevent corrupted commands sticking
          rx_index = 0;
        }
      }
    }
  }
}

esp_err_t app_driver_serial_start_rx_task(void) {
  xTaskCreate(serial_rx_task, "serial_rx", 4096, NULL, 5, NULL);
  return ESP_OK;
}

// Required but unused for GPIO in this serial implementation
esp_err_t app_driver_plugin_unit_init(const gpio_plug *plug) { return ESP_OK; }

// Called whenever Matter attribute changes
esp_err_t app_driver_attribute_update(app_driver_handle_t driver_handle,
                                      uint16_t endpoint_id, uint32_t cluster_id,
                                      uint32_t attribute_id,
                                      esp_matter_attr_val_t *val) {
  if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
    gpio_plug *plug = (gpio_plug *)driver_handle;
    if (plug && plug->index >= 1 && plug->index <= TOTAL_PLUG_COUNT) {
      int idx = plug->index;
      bool new_value = val->val.b;

      if (plug_states[idx] == new_value) {
        ESP_LOGI(TAG,
                 "Attribute update: Index %d already %s, skipping Serial TX",
                 idx, new_value ? "ON" : "OFF");
        return ESP_OK;
      }

      plug_states[idx] = new_value;

      char cmd[32];
      if (plug->is_monostable) {
        // For monostable: zcc,XX@ (no terminator)
        if (new_value) {
          snprintf(cmd, sizeof(cmd), "zcc,%02d@", idx - NUM_BISTABLE_PLUGS);
          uart_write_bytes(UART_PORT, cmd, strlen(cmd));
          ESP_LOGI(TAG, "Serial TX Monostable: %s (EP: %d, Index: %d)", cmd,
                   endpoint_id, idx);
        }
      } else {
        // For bistable: acc,XX@ (no terminator)
        snprintf(cmd, sizeof(cmd), "acc,%02d@", idx);
        uart_write_bytes(UART_PORT, cmd, strlen(cmd));
        ESP_LOGI(TAG, "Serial TX Bistable: %s (EP: %d, Index: %d, State: %s)", cmd,
                 endpoint_id, idx, new_value ? "ON" : "OFF");
      }
    }
  }
  return ESP_OK;
}

app_driver_handle_t app_driver_button_init(gpio_num_t *reset_gpio) {
  VerifyOrReturnValue((reset_gpio), (app_driver_handle_t)NULL,
                      ESP_LOGE(TAG, "reset_gpio cannot be NULL"));
  *reset_gpio = gpio_num_t::GPIO_NUM_NC;

#ifdef CONFIG_USER_BUTTON
  *reset_gpio = (gpio_num_t)CONFIG_USER_BUTTON_GPIO;
#elif CONFIG_BSP_BUTTONS_NUM >= 1
  *reset_gpio = (gpio_num_t)BSP_BUTTON_1_IO;
#endif

  if (*reset_gpio == (gpio_num_t)-1 || *reset_gpio == gpio_num_t::GPIO_NUM_NC) {
    ESP_LOGI(TAG, "Reset button skipped (GPIO_NUM_NC)");
    return NULL;
  }

  ESP_LOGI(TAG, "Initializing reset button with gpio pin %d ...",
           (int)*reset_gpio);

  // Final safety check for GPIO
  if (*reset_gpio < 0 || *reset_gpio >= GPIO_NUM_MAX) {
    ESP_LOGE(TAG, "Invalid GPIO %d for reset button", *reset_gpio);
    *reset_gpio = gpio_num_t::GPIO_NUM_NC;
    return NULL;
  }

  /* Initialize button */
  app_driver_handle_t reset_handle = NULL;
  const button_config_t btn_cfg = {0};
  const button_gpio_config_t btn_gpio_cfg = {
      .gpio_num = *reset_gpio,
      .active_level = 0, // Assume 0 as typical for BOOT button
  };

  if (iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg,
                                 (button_handle_t *)&reset_handle) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create button device");
    *reset_gpio = gpio_num_t::GPIO_NUM_NC;
    return NULL;
  }

  return reset_handle;
}
