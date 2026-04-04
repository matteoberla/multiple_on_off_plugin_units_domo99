/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include "driver/gpio.h"
#include "soc/gpio_num.h"
#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_matter_ota.h>

#include <app_priv.h>
#include <app_reset.h>
#include <common_macros.h>
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/ESP32/OpenthreadLauncher.h>
#endif

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>

static const char *TAG = "app_main";

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

constexpr auto k_timeout_seconds = 300;
uint16_t configure_plugs = 0;

// Global list (from index 0..20, 0 is unused)
plugin_endpoint plugin_unit_list[51];
// Global plug data (to use as priv_data)
static gpio_plug plug_configs[51];

static gpio_num_t reset_gpio = gpio_num_t::GPIO_NUM_NC;

#if CONFIG_ENABLE_ENCRYPTED_OTA
extern const char
    decryption_key_start[] asm("_binary_esp_image_encryption_key_pem_start");
extern const char
    decryption_key_end[] asm("_binary_esp_image_encryption_key_pem_end");

static const char *s_decryption_key = decryption_key_start;
static const uint16_t s_decryption_key_len =
    decryption_key_end - decryption_key_start;
#endif // CONFIG_ENABLE_ENCRYPTED_OTA

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg) {
  switch (event->Type) {
  case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
    ESP_LOGI(TAG, "Interface IP Address changed");
    break;

  case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
    ESP_LOGI(TAG, "Commissioning complete");
    break;

  case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
    ESP_LOGI(TAG, "Commissioning failed, fail safe timer expired");
    break;

  case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
    ESP_LOGI(TAG, "Commissioning session started");
    break;

  case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
    ESP_LOGI(TAG, "Commissioning session stopped");
    break;

  case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
    ESP_LOGI(TAG, "Commissioning window opened");
    break;

  case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
    ESP_LOGI(TAG, "Commissioning window closed");
    break;

  case chip::DeviceLayer::DeviceEventType::kFabricRemoved: {
    ESP_LOGI(TAG, "Fabric removed successfully");
    if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
      chip::CommissioningWindowManager &commissionMgr =
          chip::Server::GetInstance().GetCommissioningWindowManager();
      constexpr auto kTimeoutSeconds =
          chip::System::Clock::Seconds16(k_timeout_seconds);
      if (!commissionMgr.IsCommissioningWindowOpen()) {
        CHIP_ERROR err = commissionMgr.OpenBasicCommissioningWindow(
            kTimeoutSeconds,
            chip::CommissioningWindowAdvertisement::kDnssdOnly);
        if (err != CHIP_NO_ERROR) {
          ESP_LOGE(
              TAG,
              "Failed to open commissioning window, err:%" CHIP_ERROR_FORMAT,
              err.Format());
        }
      }
    }
    break;
  }

  case chip::DeviceLayer::DeviceEventType::kFabricWillBeRemoved:
    ESP_LOGI(TAG, "Fabric will be removed");
    break;

  case chip::DeviceLayer::DeviceEventType::kFabricUpdated:
    ESP_LOGI(TAG, "Fabric is updated");
    break;

  case chip::DeviceLayer::DeviceEventType::kFabricCommitted:
    ESP_LOGI(TAG, "Fabric is committed");
    break;

  case chip::DeviceLayer::DeviceEventType::kBLEDeinitialized:
    ESP_LOGI(TAG, "BLE deinitialized and memory reclaimed");
    break;

  default:
    break;
  }
}

static esp_err_t app_identification_cb(identification::callback_type_t type,
                                       uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant,
                                       void *priv_data) {
  ESP_LOGI(TAG, "Identification callback: type: %u, effect: %u, variant: %u",
           type, effect_id, effect_variant);
  return ESP_OK;
}

static esp_err_t
app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                        uint32_t cluster_id, uint32_t attribute_id,
                        esp_matter_attr_val_t *val, void *priv_data) {
  esp_err_t err = ESP_OK;

  if (type == PRE_UPDATE) {
    app_driver_handle_t driver_handle = (app_driver_handle_t)priv_data;
    err = app_driver_attribute_update(driver_handle, endpoint_id, cluster_id,
                                      attribute_id, val);
  }

  return err;
}

extern "C" void app_main() {
  esp_err_t err = ESP_OK;

  /* Initialize the ESP NVS layer */
  nvs_flash_init();

  /* Initialize UART */
  app_driver_serial_init();

  /* Create a Matter node and add the mandatory Root Node device type on
   * endpoint 0 */
  node::config_t node_config;
  node_t *node = node::create(&node_config, app_attribute_update_cb,
                              app_identification_cb);
  ABORT_APP_ON_FAILURE(node != nullptr,
                       ESP_LOGE(TAG, "Failed to create Matter node"));

  /* Initialize mapping list */
  for (int i = 0; i < 51; i++) {
    plugin_unit_list[i].endpoint_id = 0xFFFF;
    plugin_unit_list[i].index = i;
  }

  /* Create plugs up to 20 */
  int plugs_count = 50;
  for (int i = 0; i < plugs_count; i++) {
    int idx = i + 1;
    plug_configs[idx].index = idx;

    on_off_plug_in_unit::config_t plugin_unit_config;
    plugin_unit_config.on_off.on_off = false;

    // Passing &plug_configs[idx] as priv_data
    endpoint_t *endpoint = on_off_plug_in_unit::create(
        node, &plugin_unit_config, ENDPOINT_FLAG_NONE, &plug_configs[idx]);

    if (endpoint) {
      uint16_t endpoint_id = endpoint::get_id(endpoint);
      plugin_unit_list[idx].endpoint_id = endpoint_id;
      plugin_unit_list[idx].index = idx;
      configure_plugs++;
      ESP_LOGI(TAG, "Plug %d (index %d) created with endpoint_id %d", i + 1,
               idx, endpoint_id);
    } else {
      ESP_LOGE(TAG,
               "Failed to create endpoint for plug %d. Check Matter endpoint "
               "limits (default=16 in menuconfig).",
               idx);
      break; // Stop creating to avoid further errors
    }
  }

  // Initialize factory reset button
  app_driver_handle_t button_handle = app_driver_button_init(&reset_gpio);
  if (button_handle) {
    app_reset_button_register(button_handle);
  } else {
    ESP_LOGW(TAG, "No factory reset button configured or error during init.");
  }

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
  esp_openthread_platform_config_t config = {
      .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
      .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
      .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
  };
  set_openthread_platform_config(&config);
#endif

  /* Matter start */
  err = esp_matter::start(app_event_cb);
  ABORT_APP_ON_FAILURE(err == ESP_OK,
                       ESP_LOGE(TAG, "Failed to start Matter, err:%d", err));

  /* Start serial RX task */
  app_driver_serial_start_rx_task();

#if CONFIG_ENABLE_CHIP_SHELL
  esp_matter::console::diagnostics_register_commands();
  esp_matter::console::wifi_register_commands();
  esp_matter::console::factoryreset_register_commands();
#if CONFIG_OPENTHREAD_CLI
  esp_matter::console::otcli_register_commands();
#endif
  esp_matter::console::init();
#endif
}
