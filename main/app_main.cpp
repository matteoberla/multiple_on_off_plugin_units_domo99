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
#include "esp_wifi.h"
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
#include <app/server/Dnssd.h>
#include <app/server/Server.h>

static const char *TAG = "app_main";

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

constexpr auto k_timeout_seconds = 300;
uint16_t configure_plugs = 0;

// Global list (from index 0..TOTAL_PLUG_COUNT, 0 is unused)
plugin_endpoint plugin_unit_list[TOTAL_PLUG_COUNT + 1];
// Global plug data (to use as priv_data)
static gpio_plug plug_configs[TOTAL_PLUG_COUNT + 1];

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
    // FIX: disabilita il modem sleep WiFi per non perdere pacchetti mDNS multicast.
    // Con WIFI_PS_MIN_MODEM (default) l'ESP32 può dormire tra beacon e perdere
    // le query di scoperta operativa che HA invia dopo il suo riavvio.
    esp_wifi_set_ps(WIFI_PS_NONE);
    // FIX: forza il restart del server DNS-SD (mDNS Matter operational discovery).
    // esp-matter lo chiama già internamente, ma solo se l'IP è cambiato.
    // Con IP statico (DHCP reservation) l'IP non cambia → DnssdServer non riparte
    // → HA non riesce a riscoprire il device dopo il suo riavvio.
    chip::app::DnssdServer::Instance().StartServer();
    // Aggiorna il LED: abbiamo un IP → siamo connessi (o in commissioning se non abbiamo fabric)
    if (chip::Server::GetInstance().GetFabricTable().FabricCount() > 0) {
      app_driver_led_set_state(APP_LED_CONNECTED);
    } else {
      app_driver_led_set_state(APP_LED_COMMISSIONING);
    }
    break;

  case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
    ESP_LOGI(TAG, "Commissioning complete");
    app_driver_led_set_state(APP_LED_CONNECTED);
    break;

  case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
    ESP_LOGI(TAG, "Commissioning failed, fail safe timer expired");
    app_driver_led_set_state(APP_LED_BOOTING);
    break;

  case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
    ESP_LOGI(TAG, "Commissioning session started");
    app_driver_led_set_state(APP_LED_COMMISSIONING);
    break;

  case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
    ESP_LOGI(TAG, "Commissioning session stopped");
    break;

  case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
    ESP_LOGI(TAG, "Commissioning window opened");
    app_driver_led_set_state(APP_LED_COMMISSIONING);
    break;

  case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
    ESP_LOGI(TAG, "Commissioning window closed");
    // Se abbiamo fabric attivi torniamo a verde, altrimenti a booting
    if (chip::Server::GetInstance().GetFabricTable().FabricCount() > 0) {
      app_driver_led_set_state(APP_LED_CONNECTED);
    } else {
      app_driver_led_set_state(APP_LED_BOOTING);
    }
    break;

  case chip::DeviceLayer::DeviceEventType::kFabricRemoved: {
    ESP_LOGI(TAG, "Fabric removed successfully");
    if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
      app_driver_led_set_state(APP_LED_BOOTING);
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
    app_driver_led_set_state(APP_LED_FACTORY_RESET);
    break;

  case chip::DeviceLayer::DeviceEventType::kFabricUpdated:
    ESP_LOGI(TAG, "Fabric is updated");
    break;

  case chip::DeviceLayer::DeviceEventType::kFabricCommitted:
    ESP_LOGI(TAG, "Fabric is committed");
    app_driver_led_set_state(APP_LED_CONNECTED);
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

  /* Initialize the ESP NVS layer — con gestione robusta della corruzione.
   * Se la partizione è corrotta (ESP_ERR_NVS_NO_FREE_PAGES) o ha una versione
   * incompatibile (ESP_ERR_NVS_NEW_VERSION_FOUND), la cancelliamo e
   * reinizializziamo: evita che le credenziali WiFi vengano perse silenziosamente
   * lasciando il device impossibilitato a riconnettersi alla rete. */
  {
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_LOGW(TAG, "NVS corrotta o versione incompatibile (err=%d), cancello e reinizializzo", nvs_err);
      nvs_flash_erase();
      nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK) {
      ESP_LOGE(TAG, "Impossibile inizializzare NVS, err=%d", nvs_err);
    }
  }

  /* Initialize built-in RGB LED early so it gives visual feedback during boot */
  if (app_driver_led_init() == ESP_OK) {
    app_driver_led_set_state(APP_LED_BOOTING);
  }

  /* Initialize UART */
  app_driver_serial_init();

  /* Create a Matter node and add the mandatory Root Node device type on
   * endpoint 0 */
  node::config_t node_config;

  // Impostiamo il nome del bridge direttamente nella configurazione di
  // inizializzazione. Questo influenza l'identità del bridge fin dall'avvio.
  snprintf(node_config.root_node.basic_information.node_label,
           sizeof(node_config.root_node.basic_information.node_label),
           "Domo99 Bridge");

  node_t *node = node::create(&node_config, app_attribute_update_cb,
                              app_identification_cb);
  ABORT_APP_ON_FAILURE(node != nullptr,
                       ESP_LOGE(TAG, "Failed to create Matter node"));

  /* Impostazione Nome del Bridge (Endpoint 0) */
  endpoint_t *root_node = endpoint::get(node, 0);
  cluster_t *basic_info_0 = cluster::get(root_node, BasicInformation::Id);
  if (basic_info_0) {
    const char *bridge_name = "Domo99 Bridge";
    esp_matter_attr_val_t val =
        esp_matter_char_str((char *)bridge_name, strlen(bridge_name));
    // Cerchiamo l'attributo esistente creato da node::create()
    attribute_t *label_attr = attribute::get(
        basic_info_0, BasicInformation::Attributes::NodeLabel::Id);
    if (label_attr) {
      attribute::set_val(label_attr, &val);
    } else {
      cluster::basic_information::attribute::create_node_label(
          basic_info_0, (char *)bridge_name, strlen(bridge_name));
    }
  }



  /* Custom product and vendor info will be provided via the factory data
   * partition. See the instructions on using mfg_tool.py to generate the
   * 'fctry' partition. */

  /* Initialize mapping list */
  for (int i = 0; i < TOTAL_PLUG_COUNT + 1; i++) {
    plugin_unit_list[i].endpoint_id = 0xFFFF;
    plugin_unit_list[i].index = i;
  }

  /* Toggle for Aggregator endpoint structure */
  bool use_endpoint_aggregator = true;
  endpoint_t *aggregator = NULL;

  if (use_endpoint_aggregator) {
    /* Create an Aggregator endpoint (Bridge) to allow unique naming in Apple
     * Home */
    endpoint::aggregator::config_t aggregator_config;
    aggregator = endpoint::aggregator::create(node, &aggregator_config,
                                              ENDPOINT_FLAG_NONE, NULL);
    ABORT_APP_ON_FAILURE(aggregator != nullptr,
                         ESP_LOGE(TAG, "Failed to create Aggregator endpoint"));
  }

  int num_bistable = NUM_BISTABLE_PLUGS;
  int num_monostable = NUM_MONOSTABLE_PLUGS;
  int total_plugs = num_bistable + num_monostable;

  for (int i = 0; i < total_plugs; i++) {
    int idx = i + 1;
    plug_configs[idx].index = idx;
    // Indices after num_bistable are monostable
    plug_configs[idx].is_monostable = (idx > num_bistable);

    on_off_plug_in_unit::config_t plugin_unit_config;
    plugin_unit_config.on_off.on_off = false;

    // Passing &plug_configs[idx] as priv_data
    endpoint_t *endpoint = on_off_plug_in_unit::create(
        node, &plugin_unit_config, ENDPOINT_FLAG_NONE, &plug_configs[idx]);

    if (endpoint) {
      uint16_t endpoint_id = endpoint::get_id(endpoint);

      /* Add unique name (Node Label) and Serial */
      static char ep_names[TOTAL_PLUG_COUNT + 1][16];
      static char ep_serials[TOTAL_PLUG_COUNT + 1][16];

      if (plug_configs[idx].is_monostable) {
        snprintf(ep_names[idx], sizeof(ep_names[idx]), "M%02d",
                 idx - num_bistable);
        snprintf(ep_serials[idx], sizeof(ep_serials[idx]), "D99-M%02d",
                 idx - num_bistable);
      } else {
        snprintf(ep_names[idx], sizeof(ep_names[idx]), "B%02d", idx);
        snprintf(ep_serials[idx], sizeof(ep_serials[idx]), "D99-B%02d", idx);
      }

      /* 1. Definiamo l'endpoint come Bridged Node (0x0013) solo se l'aggregator è attivo */
      if (use_endpoint_aggregator && aggregator) {
        endpoint::add_device_type(endpoint, 0x0013, 1);
        endpoint::set_parent_endpoint(endpoint, aggregator);

        /* 2. Creazione Cluster Metadati - Solo per nodi bridged */
        cluster::bridged_device_basic_information::config_t bridged_config;
        cluster_t *bridged_cluster =
            cluster::bridged_device_basic_information::create(
                endpoint, &bridged_config, CLUSTER_FLAG_SERVER);
        
        if (bridged_cluster) {
          cluster::bridged_device_basic_information::attribute::create_node_label(bridged_cluster, ep_names[idx], strlen(ep_names[idx]));
          cluster::bridged_device_basic_information::attribute::create_vendor_name(bridged_cluster, (char*)"Elettronica Mimmo", 17);
          cluster::bridged_device_basic_information::attribute::create_product_name(bridged_cluster, (char*)"Domo99", 6);
          cluster::bridged_device_basic_information::attribute::create_serial_number(bridged_cluster, ep_serials[idx], strlen(ep_serials[idx]));
          cluster::bridged_device_basic_information::attribute::create_reachable(bridged_cluster, true);
        }
      }

      ESP_LOGI(TAG, "Endpoint %d (Plug %d) configurato (Label: %s, Serial: %s)",
               endpoint_id, idx, ep_names[idx], ep_serials[idx]);

      plugin_unit_list[idx].endpoint_id = endpoint_id;


      plugin_unit_list[idx].index = idx;
      configure_plugs++;
      ESP_LOGI(TAG,
               "Plug %d (index %d, monostable %d) created with endpoint_id %d",
               i + 1, idx, plug_configs[idx].is_monostable, endpoint_id);
    } else {
      ESP_LOGE(TAG,
               "Failed to create endpoint for plug %d. Check Matter endpoint "
               "limits.",
               idx);
      break;
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
