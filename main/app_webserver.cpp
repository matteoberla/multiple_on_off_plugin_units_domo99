/*
 * Web server HTTP opzionale (CONFIG_ENABLE_WEBSERVER).
 *
 *   GET /AXX  -> invia acc,XX@  (toggle uscita bistabile)
 *   GET /ZXX  -> invia zcc,XX@  (impulso uscita monostabile)
 *   GET /     -> elenco "LEDxxON/OFF" (LEDZxx per i monostabili) in un blocco
 *                trasparente, leggibile solo con "ispeziona".
 *
 * Lo stato mostrato è quello confermato dalla seriale (on,XX / of,XX ...).
 * Nessuna autenticazione.
 */
#include <app_priv.h>
#include <esp_log.h>

#if CONFIG_ENABLE_WEBSERVER

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <esp_http_server.h>

static const char *TAG = "webserver";
static httpd_handle_t s_server = NULL;

static esp_err_t send_state_page(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_sendstr_chunk(
      req, "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
           "<title>Domo99</title></head><body>"
           "<div id=\"states\" style=\"color:transparent;background:transparent;"
           "user-select:none\">");
  char line[48];
  for (int i = 1; i <= NUM_BISTABLE_PLUGS; i++) {
    snprintf(line, sizeof(line), "<div>LED%02d%s</div>", i,
             app_driver_get_confirmed_state(i) ? "ON" : "OFF");
    httpd_resp_sendstr_chunk(req, line);
  }
  for (int i = 1; i <= NUM_MONOSTABLE_PLUGS; i++) {
    snprintf(line, sizeof(line), "<div>LEDZ%02d%s</div>", i,
             app_driver_get_confirmed_state(NUM_BISTABLE_PLUGS + i) ? "ON"
                                                                    : "OFF");
    httpd_resp_sendstr_chunk(req, line);
  }
  httpd_resp_sendstr_chunk(req, "</div></body></html>");
  return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t root_handler(httpd_req_t *req) {
  const char *uri = req->uri;
  if (strcmp(uri, "/") == 0) {
    return send_state_page(req);
  }
  // /AXX o /ZXX, con eventuale query string ignorata
  char type = (char)toupper((unsigned char)uri[1]);
  if ((type == 'A' || type == 'Z') && isdigit((unsigned char)uri[2])) {
    char *end = NULL;
    long num = strtol(uri + 2, &end, 10);
    if (end && (*end == '\0' || *end == '?' || *end == '/')) {
      if (app_driver_send_web_command(type, (int)num) == ESP_OK) {
        char resp[16];
        snprintf(resp, sizeof(resp), "OK %c%02ld", type, num);
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, resp);
      }
    }
  }
  return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
}

void app_webserver_start(void) {
  if (s_server) return;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.max_uri_handlers = 2;
  config.lru_purge_enable = true;
  if (httpd_start(&s_server, &config) != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed");
    s_server = NULL;
    return;
  }
  httpd_uri_t uri = {};
  uri.uri = "/*";
  uri.method = HTTP_GET;
  uri.handler = root_handler;
  httpd_register_uri_handler(s_server, &uri);
  ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
}

#else

void app_webserver_start(void) {}

#endif
