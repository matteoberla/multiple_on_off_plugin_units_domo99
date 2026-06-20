/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include "soc/gpio_num.h"
#include <esp_err.h>
#include <esp_matter.h>

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include "esp_openthread_types.h"
#endif

// This struct is used as priv_data for each endpoint.
// Instead of a GPIO pin, it stores the plug index (1-20).
struct gpio_plug {
  int index;
  bool is_monostable;
};

struct plugin_endpoint {
  uint16_t endpoint_id;
  int index;
};

// Configurazione Manuale
// Per testare tutte le uscite bistabili 1-94: 94 bistabili, 0 monostabili.
#define NUM_BISTABLE_PLUGS 94
#define NUM_MONOSTABLE_PLUGS 0
#define TOTAL_PLUG_COUNT (NUM_BISTABLE_PLUGS + NUM_MONOSTABLE_PLUGS)

// Size for index 0 to TOTAL_PLUG_COUNT
extern plugin_endpoint plugin_unit_list[TOTAL_PLUG_COUNT + 1];
extern uint16_t configure_plugs;

typedef void *app_driver_handle_t;

// Standard plug init (restored signature)
esp_err_t app_driver_plugin_unit_init(const gpio_plug *plug);

/** Driver Update
 *
 * This API should be called to update the driver for the attribute being
 * updated. This is usually called from the common `app_attribute_update_cb()`.
 */
esp_err_t app_driver_attribute_update(app_driver_handle_t driver_handle,
                                      uint16_t endpoint_id, uint32_t cluster_id,
                                      uint32_t attribute_id,
                                      esp_matter_attr_val_t *val);

/** Initialize the button driver
 */
app_driver_handle_t app_driver_button_init(gpio_num_t *reset_gpio);

/** LED RGB states for visual feedback */
typedef enum {
  APP_LED_BOOTING = 0,       /**< Avvio: blu lento lampeggiante */
  APP_LED_COMMISSIONING,     /**< Commissioning BLE: azzurro veloce lampeggiante */
  APP_LED_CONNECTED,         /**< Connesso a Matter fabric: verde fisso */
  APP_LED_FACTORY_RESET,     /**< Factory reset in corso: rosso lampeggiante */
  APP_LED_ERROR,             /**< Errore: rosso fisso */
  APP_LED_OFF,               /**< LED spento */
} app_led_state_t;

/** Initialize the built-in RGB LED (WS2812 via RMT)
 *  @return ESP_OK on success
 */
esp_err_t app_driver_led_init(void);

/** Set the LED to a specific visual state.
 *  This function is safe to call from any task.
 *  @param state  The desired visual state
 */
void app_driver_led_set_state(app_led_state_t state);

// Serial functions
esp_err_t app_driver_serial_init(void);
esp_err_t app_driver_serial_start_rx_task(void);

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#define ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG()                                  \
  {                                                                            \
      .radio_mode = RADIO_MODE_NATIVE,                                         \
  }

#define ESP_OPENTHREAD_DEFAULT_HOST_CONFIG()                                   \
  {                                                                            \
      .host_connection_mode = HOST_CONNECTION_MODE_NONE,                       \
  }

#define ESP_OPENTHREAD_DEFAULT_PORT_CONFIG()                                   \
  {                                                                            \
      .storage_partition_name = "nvs",                                         \
      .netif_queue_size = 10,                                                  \
      .task_queue_size = 10,                                                   \
  }
#endif
