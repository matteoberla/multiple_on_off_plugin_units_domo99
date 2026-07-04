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

// LED Strip (WS2812 built-in RGB)
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <app_priv.h>
#include <button_gpio.h>
#include <esp_matter.h>
#include <iot_button.h>

using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "app_driver";

// ---------------------------------------------------------------------------
// RGB LED built-in driver (WS2812 via RMT)
// ---------------------------------------------------------------------------

#ifndef CONFIG_RGB_LED_GPIO
#define CONFIG_RGB_LED_GPIO 48
#endif

// LED brightness (0-255). Keep it low to avoid blinding.
#define LED_BRIGHTNESS 30

static led_strip_handle_t s_led_strip = NULL;
static QueueHandle_t      s_led_queue = NULL;
// Stato LED precedente al factory reset — usato dal callback BUTTON_PRESS_UP
static app_led_state_t    s_led_pre_reset_state = APP_LED_BOOTING;

/** Set a single solid colour on the LED (r,g,b scaled to LED_BRIGHTNESS) */
static void led_set_color(uint8_t r, uint8_t g, uint8_t b)
{
  if (!s_led_strip) return;
  // Scale
  r = (uint8_t)((r * LED_BRIGHTNESS) / 255);
  g = (uint8_t)((g * LED_BRIGHTNESS) / 255);
  b = (uint8_t)((b * LED_BRIGHTNESS) / 255);
  led_strip_set_pixel(s_led_strip, 0, r, g, b);
  led_strip_refresh(s_led_strip);
}

/** FreeRTOS task that owns the LED animation loop */
static void led_task(void *arg)
{
  app_led_state_t current_state = APP_LED_BOOTING;
  app_led_state_t new_state;
  bool led_on = false;

  while (true) {
    // Check for a new state (non-blocking)
    if (xQueueReceive(s_led_queue, &new_state, 0) == pdTRUE) {
      current_state = new_state;
      led_on = false;
      // Immediately clear LED when switching state
      led_strip_clear(s_led_strip);
      led_strip_refresh(s_led_strip);
    }

    switch (current_state) {
      case APP_LED_BOOTING:
        // Blu lento lampeggiante (1 Hz)
        led_on = !led_on;
        if (led_on) led_set_color(0, 0, 255);
        else { led_strip_clear(s_led_strip); led_strip_refresh(s_led_strip); }
        vTaskDelay(pdMS_TO_TICKS(500));
        break;

      case APP_LED_COMMISSIONING:
        // Azzurro veloce lampeggiante (4 Hz)
        led_on = !led_on;
        if (led_on) led_set_color(0, 100, 255);
        else { led_strip_clear(s_led_strip); led_strip_refresh(s_led_strip); }
        vTaskDelay(pdMS_TO_TICKS(125));
        break;

      case APP_LED_CONNECTED:
        // Verde fisso
        led_set_color(0, 255, 0);
        vTaskDelay(pdMS_TO_TICKS(1000));
        break;

      case APP_LED_FACTORY_RESET:
        // Rosso lampeggiante veloce (8 Hz)
        led_on = !led_on;
        if (led_on) led_set_color(255, 0, 0);
        else { led_strip_clear(s_led_strip); led_strip_refresh(s_led_strip); }
        vTaskDelay(pdMS_TO_TICKS(125));
        break;

      case APP_LED_ERROR:
        // Rosso fisso
        led_set_color(255, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(1000));
        break;

      case APP_LED_OFF:
      default:
        led_strip_clear(s_led_strip);
        led_strip_refresh(s_led_strip);
        vTaskDelay(pdMS_TO_TICKS(500));
        break;
    }
  }
}

esp_err_t app_driver_led_init(void)
{
  led_strip_config_t strip_config = {
    .strip_gpio_num          = CONFIG_RGB_LED_GPIO,
    .max_leds                = 1,
    .led_model               = LED_MODEL_WS2812,
    .color_component_format  = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    .flags                   = { .invert_out = 0 },
  };
  led_strip_rmt_config_t rmt_config = {
    .clk_src        = RMT_CLK_SRC_DEFAULT,
    .resolution_hz  = 10 * 1000 * 1000, // 10 MHz
    .mem_block_symbols = 64,
    .flags          = { .with_dma = false },
  };

  esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create LED strip: %d", err);
    return err;
  }

  // Turn off at boot
  led_strip_clear(s_led_strip);
  led_strip_refresh(s_led_strip);

  // Create command queue (depth=1, required by xQueueOverwrite) and task
  // xQueueOverwrite requires a queue of length exactly 1
  s_led_queue = xQueueCreate(1, sizeof(app_led_state_t));
  if (!s_led_queue) {
    ESP_LOGE(TAG, "Failed to create LED queue");
    return ESP_ERR_NO_MEM;
  }

  xTaskCreate(led_task, "led_task", 2048, NULL, 3, NULL);
  ESP_LOGI(TAG, "RGB LED initialized on GPIO%d", CONFIG_RGB_LED_GPIO);
  return ESP_OK;
}

void app_driver_led_set_state(app_led_state_t state)
{
  if (s_led_queue) {
    // Salva lo stato precedente ogni volta che non stiamo andando in FACTORY_RESET
    // così il callback BUTTON_PRESS_UP può ripristinare l'ultimo stato stabile.
    if (state != APP_LED_FACTORY_RESET) {
      s_led_pre_reset_state = state;
    }
    xQueueOverwrite(s_led_queue, &state);
  }
}

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
      .baud_rate = 19200,
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

// ---------------------------------------------------------------------------
// Helper: parse and apply a single "on,XX" or "of,XX" token to Matter state
// Returns true if a valid command was found and processed.
// ---------------------------------------------------------------------------
static bool parse_and_apply_state(const char *token) {
  const char *p = NULL;
  bool new_state = false;
  bool found = false;
  bool is_monostable_cmd = false;

  if ((p = strstr(token, "on,")) != NULL) {
    new_state = true;
    found = true;
    is_monostable_cmd = false;
  } else if ((p = strstr(token, "of,")) != NULL) {
    new_state = false;
    found = true;
    is_monostable_cmd = false;
  } else if ((p = strstr(token, "za,")) != NULL) {
    new_state = true;
    found = true;
    is_monostable_cmd = true;
  } else if ((p = strstr(token, "zs,")) != NULL) {
    new_state = false;
    found = true;
    is_monostable_cmd = true;
  }

  if (!found) return false;

  int raw_idx = atoi(p + 3);
  int idx = is_monostable_cmd ? (raw_idx + NUM_BISTABLE_PLUGS) : raw_idx;

  ESP_LOGI(TAG, "parse_and_apply_state: Raw Index %d, Final Index %d, State: %s",
           raw_idx, idx, new_state ? "ON" : "OFF");

  if (idx >= 1 && idx <= TOTAL_PLUG_COUNT) {
    uint16_t ep_id = plugin_unit_list[idx].endpoint_id;
    if (ep_id != 0xFFFF) {
      if (plug_states[idx] != new_state) {
        plug_states[idx] = new_state;
        esp_matter_attr_val_t val = esp_matter_bool(new_state);
        ESP_LOGI(TAG, "parse_and_apply_state: Updating Attribute EP %d to %s",
                 ep_id, new_state ? "ON" : "OFF");
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        esp_err_t err = attribute::update(
            ep_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &val);
        if (err != ESP_OK) {
          ESP_LOGE(TAG, "parse_and_apply_state: Failed to update EP %d, err: %d",
                   ep_id, err);
        }
      } else {
        ESP_LOGI(TAG, "parse_and_apply_state: Index %d already %s, skipping",
                 idx, new_state ? "ON" : "OFF");
      }
    } else {
      ESP_LOGW(TAG, "parse_and_apply_state: Index %d has no endpoint ID", idx);
    }
  } else {
    ESP_LOGW(TAG, "parse_and_apply_state: Invalid index %d", idx);
  }
  return true;
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

          // La risposta a acc,00@ arriva come "on,01|on,05|on,06" (separata da '|').
          // Un comando normale arriva come singolo token "on,XX" o "of,XX".
          // In entrambi i casi il terminatore è 0x0d: tokenizziamo su '|' e
          // processiamo ogni token con l'helper parse_and_apply_state().
          char *saveptr = NULL;
          char *token = strtok_r(rx_buffer, "|", &saveptr);
          while (token != NULL) {
            parse_and_apply_state(token);
            token = strtok_r(NULL, "|", &saveptr);
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

/**
 * @brief Invia il comando acc,00@ al dispositivo seriale.
 *
 * Questo comando speciale chiede al dispositivo di rispondere con tutti gli
 * stati ON attualmente attivi, in formato "on,01|on,05|on,06\r".
 * Va chiamato dopo che il sistema è completamente inizializzato (Matter avviato
 * e RX task in esecuzione) in modo che la risposta venga correttamente
 * processata dal serial_rx_task.
 */
esp_err_t app_driver_serial_send_query_all(void) {
  const char *cmd = "acc,00@";
  int written = uart_write_bytes(UART_PORT, cmd, strlen(cmd));
  if (written < 0) {
    ESP_LOGE(TAG, "app_driver_serial_send_query_all: uart_write_bytes failed");
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "Serial TX Query-All: %s (%d bytes)", cmd, written);
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

  // Final safety check for GPIO
  if (*reset_gpio < 0 || *reset_gpio >= GPIO_NUM_MAX) {
    ESP_LOGE(TAG, "Invalid GPIO %d for reset button", *reset_gpio);
    *reset_gpio = gpio_num_t::GPIO_NUM_NC;
    return NULL;
  }

#ifdef CONFIG_USER_BUTTON_LEVEL
  int active_level = CONFIG_USER_BUTTON_LEVEL;
#else
  int active_level = 0;
#endif

  ESP_LOGI(TAG, "Initializing reset button on GPIO%d (active_level=%d)",
           (int)*reset_gpio, active_level);

  /* Initialize button */
  app_driver_handle_t reset_handle = NULL;
  const button_config_t btn_cfg = {0};
  const button_gpio_config_t btn_gpio_cfg = {
      .gpio_num    = *reset_gpio,
      .active_level = (uint8_t)active_level,
  };

  if (iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg,
                                 (button_handle_t *)&reset_handle) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create button device on GPIO%d", (int)*reset_gpio);
    *reset_gpio = gpio_num_t::GPIO_NUM_NC;
    return NULL;
  }

  // ------------------------------------------------------------------
  // LED feedback callbacks — registered directly on the button handle.
  // app_reset uses BUTTON_LONG_PRESS_HOLD + BUTTON_PRESS_UP internally;
  // we piggyback the same events to drive the RGB LED.
  // ------------------------------------------------------------------

  // Accende il LED rosso non appena la soglia long-press è raggiunta
  iot_button_register_cb(
      (button_handle_t)reset_handle,
      BUTTON_LONG_PRESS_HOLD, NULL,
      [](void * /*btn*/, void * /*data*/) {
        ESP_LOGI("app_driver", "Reset button held — LED rosso attivo");
        app_driver_led_set_state(APP_LED_FACTORY_RESET);
      },
      NULL);

  // Al rilascio: ripristina lo stato LED precedente.
  // Se il reset è stato avviato il device si riavvierà comunque,
  // altrimenti il LED torna allo stato pre-pressione (CONNECTED o BOOTING).
  iot_button_register_cb(
      (button_handle_t)reset_handle,
      BUTTON_PRESS_UP, NULL,
      [](void * /*btn*/, void * /*data*/) {
        ESP_LOGI("app_driver", "Reset button released — ripristino stato LED");
        // s_led_pre_reset_state viene aggiornato da app_driver_led_set_state
        // ogni volta che lo stato cambia (prima di impostare FACTORY_RESET)
        app_driver_led_set_state(s_led_pre_reset_state);
      },
      NULL);


  ESP_LOGI(TAG, "Reset button ready on GPIO%d — hold 3s to factory reset",
           (int)*reset_gpio);
  return reset_handle;
}
