#include <string.h>
#include <sys/param.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_sleep.h"
#include "esp_netif.h"

#include "tcpip_adapter.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/api.h"
#include "lwip/err.h"
#include "lwip/dns.h"

#include "cJSON.h"

#include "common_config.h"
#include "local_webserver_apsta.h"
#include "captdns.h"

#define WIFI_CONNECTED_BIT      BIT0
#define WIFI_FAIL_BIT           BIT1
#define WIFI_STARTED_BIT        BIT2
#define STA_CONNECTED_BIT       BIT3
#define STA_DISCONNECTED_BIT    BIT4

#define ASCII_START             32
#define ASCII_END               126

static const char *TAG = "local_webserver_apsta";

const static char http_html_hdr[] = "HTTP/1.1 200 OK\r\nContent-type: text/html\r\n\r\n";
const uint8_t indexHtmlStart[] asm("_binary_index_html_start");
const uint8_t indexHtmlEnd[] asm("_binary_index_html_end");
const uint8_t Wifi1SvgStart[] asm("_binary_wifi1_svg_start");
const uint8_t Wifi1SvgEnd[] asm("_binary_wifi1_svg_end");
const uint8_t Wifi2SvgStart[] asm("_binary_wifi2_svg_start");
const uint8_t Wifi2SvgEnd[] asm("_binary_wifi2_svg_end");
const uint8_t Wifi3SvgStart[] asm("_binary_wifi3_svg_start");
const uint8_t Wifi3SvgEnd[] asm("_binary_wifi3_svg_end");
const uint8_t WifiFullSvgStart[] asm("_binary_wififull_svg_start");
const uint8_t WifiFullSvgEnd[] asm("_binary_wififull_svg_end");

extern SemaphoreHandle_t xSemaphore_app;

static httpd_handle_t server = NULL;

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

static ip4_addr_t s_ip_addr;

static int s_retry_num = 0;

static char __SSID[32];
static char __PASSWORD[64];
volatile char __DEVICEID[128];

static volatile bool connected = true;

static void init_ap_config(void);
static void restart_sta(char *ssid, char *password);

char *randstring(int length) {    
    char *string = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    size_t stringLen = 26*2+10;        
    char *randomString;

    randomString = malloc(sizeof(char) * (length +1));

    if (!randomString) {
        return (char*)0;
    }

    unsigned int key = 0;

    for (int n = 0;n < length;n++) {            
        key = esp_random() % stringLen;
        randomString[n] = string[key];
    }

    randomString[length] = '\0';

    return randomString;
}

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    ESP_LOGI(TAG,"event_handler  %s  %X", event_base, event_id);
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < STA_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            start_webserver();
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        if (!connected) {
            esp_restart();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        memcpy(&s_ip_addr, &event->ip_info.ip, sizeof(s_ip_addr));
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&s_ip_addr));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        xSemaphoreGive(xSemaphore_app);
        if (!connected) {
            esp_restart();
        }
    } else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
}

static void init_static_ip(void) {
	// stop DHCP server
	ESP_ERROR_CHECK(tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP));

	// assign a static IP to the network interface
	tcpip_adapter_ip_info_t info;
    memset(&info, 0, sizeof(info));
	IP4_ADDR(&info.ip, 192, 168, 99, 1);
    IP4_ADDR(&info.gw, 192, 168, 99, 1);
    IP4_ADDR(&info.netmask, 255, 255, 255, 0);
	ESP_ERROR_CHECK(tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_AP, &info));

    // start the DHCP server
    ESP_ERROR_CHECK(tcpip_adapter_dhcps_start(TCPIP_ADAPTER_IF_AP));
}

static void init_ap_config(void) {
    ESP_LOGI(TAG, "init_ap_config");
    wifi_config_t ap_config = {
        .ap = {
            .ssid = AP_WIFI_SSID,
            .ssid_len = strlen(AP_WIFI_SSID),
            .password = AP_WIFI_PASS,
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };
    if (strlen(AP_WIFI_PASS) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    char UUID_SSID[20];
    sprintf(UUID_SSID, "%s%s", AP_WIFI_SSID, randstring(4));

    if (strlen(UUID_SSID) != 0) {
        strcpy((char *) ap_config.ap.ssid, UUID_SSID);
        ap_config.ap.ssid_len = strlen(UUID_SSID);
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config));

    ESP_LOGI(TAG, "ssid: %s  %d", UUID_SSID, strlen(UUID_SSID));
    ESP_LOGI(TAG, "password: %s  %d", AP_WIFI_PASS, strlen(AP_WIFI_PASS));
}

static void init_sta_config(char *ssid, char *password) {
    ESP_LOGI(TAG, "init_sta_config");
    wifi_config_t sta_config = {
        .sta = {
            .ssid = "",
            .password = "",
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	     .threshold.authmode = WIFI_AUTH_WPA2_PSK
        },
    };

    if (strlen(ssid) != 0) {
        strcpy((char *)sta_config.sta.ssid, ssid);
        strcpy((char *)sta_config.sta.password, password);
    }
    
	ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &sta_config));

    ESP_LOGI(TAG, "ssid: %s  %d", sta_config.sta.ssid, strlen(ssid));
    ESP_LOGI(TAG, "password: %s  %d", sta_config.sta.password, strlen(password));
}

void start_wifi_service(void)
{
    s_wifi_event_group = xEventGroupCreate();
    // xTaskCreate(&monitor_task, "monitor_task", 4096, NULL, 5, NULL);

    // disable the default wifi logging
	esp_log_level_set("wifi", ESP_LOG_NONE);

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

	ESP_ERROR_CHECK(esp_netif_init());

    // initialize the tcp stack
	tcpip_adapter_init();
    
    init_static_ip();

	// initialize the wifi event handler
	// ESP_ERROR_CHECK(esp_event_loop_init(event_handler_apsta, NULL));

	// initialize the WiFi stack in AccessPoint mode with configuration in RAM
	wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t cfg;
    esp_wifi_get_config(WIFI_IF_STA, &cfg);
    sprintf(__SSID, "%s", cfg.sta.ssid);
    sprintf(__PASSWORD, "%s", cfg.sta.password);

    init_sta_config(__SSID, __PASSWORD);

	ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "start_wifi_service finished.");
}

static void restart_sta(char *ssid, char *password) {
    ESP_ERROR_CHECK(esp_wifi_stop());
	init_sta_config(ssid, password);
	ESP_ERROR_CHECK(esp_wifi_start());
}

/*----------------------------------------------------------------------------*/

/* This handler allows the custom error handling functionality to be
 * tested from client side. For that, when a PUT request 0 is sent to
 * URI /ctrl, the /hello and /echo URIs are unregistered and following
 * custom error handler http_404_error_handler() is registered.
 * Afterwards, when /hello or /echo is requested, this custom error
 * handler is invoked which, after sending an error message to client,
 * either closes the underlying socket (when requested URI is /echo)
 * or keeps it open (when requested URI is /hello). This allows the
 * client to infer if the custom error handler is functioning as expected
 * by observing the socket state.
 */

static esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    if (strcmp("/hello", req->uri) == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "/hello URI is not available");
        /* Return ESP_OK to keep underlying socket open */
        return ESP_OK;
    } else if (strcmp("/echo", req->uri) == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "/echo URI is not available");
        /* Return ESP_FAIL to close underlying socket */
        return ESP_FAIL;
    }
    /* For any other URI send 404 and close socket */
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Some 404 error message");
    return ESP_FAIL;
}

static esp_err_t servePage_get_handler(httpd_req_t *req)
{
    // httpd_resp_send(req, (char *)http_html_hdr, sizeof(http_html_hdr)-1);
    httpd_resp_send(req, (char *)indexHtmlStart, indexHtmlEnd-indexHtmlStart);
    return ESP_OK;
}

static const httpd_uri_t servePage = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = servePage_get_handler,
    .user_ctx = NULL
};

static const httpd_uri_t CaptivePortal = {
    .uri = "/hotspot-detect.html",
    .method = HTTP_GET,
    .handler = servePage_get_handler,
    .user_ctx = NULL
};

static esp_err_t wifi1_svg_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_send(req, (char *)Wifi1SvgStart, Wifi1SvgEnd - Wifi1SvgStart);
    return ESP_OK;
}

static const httpd_uri_t wifi1_svg = {
    .uri = "/wifi1.svg",
    .method = HTTP_GET,
    .handler = wifi1_svg_get_handler,
    .user_ctx = NULL
};

static esp_err_t wifi2_svg_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_send(req, (char *)Wifi2SvgStart, Wifi2SvgEnd - Wifi2SvgStart);
    return ESP_OK;
}

static const httpd_uri_t wifi2_svg = {
    .uri = "/wifi2.svg",
    .method = HTTP_GET,
    .handler = wifi2_svg_get_handler,
    .user_ctx = NULL
};

static esp_err_t wifi3_svg_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_send(req, (char *)Wifi3SvgStart, Wifi3SvgEnd - Wifi3SvgStart);
    return ESP_OK;
}

static const httpd_uri_t wifi3_svg = {
    .uri = "/wifi3.svg",
    .method = HTTP_GET,
    .handler = wifi3_svg_get_handler,
    .user_ctx = NULL
};

static esp_err_t wififull_svg_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_send(req, (char *)WifiFullSvgStart, WifiFullSvgEnd - WifiFullSvgStart);
    return ESP_OK;
}

static const httpd_uri_t wififull_svg = {
    .uri = "/wififull.svg",
    .method = HTTP_GET,
    .handler = wififull_svg_get_handler,
    .user_ctx = NULL
};

static esp_err_t wifiinfo_get_handler(httpd_req_t *req)
{
    char buf[128];
    int ret, remaining = req->content_len;
    // char *ssid = NULL, *password = NULL;
    while (remaining > 0)
    {
        /* Read the data for the request */
        if ((ret = httpd_req_recv(req, buf,
                                  MIN(remaining, sizeof(buf)))) <= 0)
        {
            if (ret == 0)
            {
                ESP_LOGI(TAG, "No content received please try again ...");
            }
            else if (ret == HTTPD_SOCK_ERR_TIMEOUT)
            {

                /* Retry receiving if timeout occurred */
                continue;
            }
            return ESP_FAIL;
        }

        /* Log data received */
        /* ESP_LOGI(TAG, "=========== RECEIVED DATA ==========");
        ESP_LOGI(TAG, "%.*s", ret, buf);
        ESP_LOGI(TAG, "===================================="); */
        cJSON *root = cJSON_Parse(buf);
        const cJSON *json_tmp = NULL;
        char *string = cJSON_Print(root);
        ESP_LOGI(TAG, "%s", string);

        // json_tmp = cJSON_GetObjectItemCaseSensitive(root, "ssid");
        // ssid = cJSON_Print(json_tmp);

        // json_tmp = cJSON_GetObjectItemCaseSensitive(root, "password");
        // password = cJSON_Print(json_tmp);

        sprintf(__SSID, "%s", cJSON_GetObjectItem(root, "ssid")->valuestring);
        sprintf(__PASSWORD, "%s", cJSON_GetObjectItem(root, "password")->valuestring);

        remaining -= ret;
        free(string);
        cJSON_Delete(root);
    }
    
    // End response
    ESP_LOGI(TAG, "End response");
    httpd_resp_send_chunk(req, NULL, 0);
    restart_sta(__SSID, __PASSWORD);
    return ESP_OK;
}

static const httpd_uri_t wifiinfo = {
    .uri = "/wifiinfo",
    .method = HTTP_POST,
    .handler = wifiinfo_get_handler
};

static esp_err_t scanwifi_get_handler(httpd_req_t *req)
{
    uint16_t number = DEFAULT_SCAN_LIST_SIZE;
    wifi_ap_record_t ap_info[DEFAULT_SCAN_LIST_SIZE];
    uint16_t ap_count = 0;
    memset(ap_info, 0, sizeof(ap_info));
    char *string = NULL;
    cJSON *wifilist = cJSON_CreateObject();
    cJSON *lists = NULL;
    cJSON *list = NULL;
    cJSON *ssid = NULL;
    cJSON *rssi = NULL;

    ESP_LOGI(TAG, "/scanwifi");

    lists = cJSON_CreateArray();
    if (lists == NULL) {
        // goto end;
    }
    cJSON_AddItemToObject(wifilist, "lists", lists);

    ESP_ERROR_CHECK(esp_wifi_scan_start(NULL, true));
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&number, ap_info));
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
    ESP_LOGI(TAG, "Total APs scanned = %u", ap_count);
    for (int i = 0; (i < DEFAULT_SCAN_LIST_SIZE) && (i < ap_count); i++) {
        list = cJSON_CreateObject();
        if (list == NULL)
        {
            // goto end;
        }
        cJSON_AddItemToArray(lists, list);

        ssid = cJSON_CreateString((char *) ap_info[i].ssid);
        if (ssid == NULL) {
            // goto end;
        }
        cJSON_AddItemToObject(list, "ssid", ssid);

        rssi = cJSON_CreateNumber(ap_info[i].rssi);
        if (rssi == NULL) {
            // goto end;
        }
        cJSON_AddItemToObject(list, "rssi", rssi);
    }

    string = cJSON_Print(wifilist);

    // End response
    // httpd_resp_send_chunk(req, NULL, 0);
    httpd_resp_sendstr(req, string);
    free(string);
    cJSON_Delete(wifilist);
    return ESP_OK;
}

static const httpd_uri_t scanwifi = {
    .uri = "/scanwifi",
    .method = HTTP_GET,
    .handler = scanwifi_get_handler
};

static esp_err_t setdeviceid_get_handler(httpd_req_t *req)
{
    char buf[128];
    int ret, remaining = req->content_len;
    // char *ssid = NULL, *password = NULL;
    while (remaining > 0)
    {
        /* Read the data for the request */
        if ((ret = httpd_req_recv(req, buf,
                                  MIN(remaining, sizeof(buf)))) <= 0)
        {
            if (ret == 0)
            {
                ESP_LOGI(TAG, "No content received please try again ...");
            }
            else if (ret == HTTPD_SOCK_ERR_TIMEOUT)
            {

                /* Retry receiving if timeout occurred */
                continue;
            }
            return ESP_FAIL;
        }

        /* Log data received */
        /* ESP_LOGI(TAG, "=========== RECEIVED DATA ==========");
        ESP_LOGI(TAG, "%.*s", ret, buf);
        ESP_LOGI(TAG, "===================================="); */
        cJSON *root = cJSON_Parse(buf);
        const cJSON *json_tmp = NULL;
        char *string = cJSON_Print(root);
        ESP_LOGI(TAG, "%s", string);

        sprintf(__DEVICEID, "%s", cJSON_GetObjectItem(root, "deviceid")->valuestring);
        ESP_LOGI(TAG, "deviceid: %s", __DEVICEID);

        esp_err_t err;
        nvs_handle_t nvs_handle;
        ESP_LOGI(TAG, "Open storage");
        err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK)
        {
            ESP_LOGI(TAG, "Error (%s) opening NVS handle!\n", esp_err_to_name(err));
        }
        else
        {
            err = nvs_set_str(nvs_handle, "deviceid", __DEVICEID);
            printf((err != ESP_OK) ? "Failed!\n" : "Done\n");
            err = nvs_commit(nvs_handle);
            printf((err != ESP_OK) ? "Failed!\n" : "Done\n");
            nvs_close(nvs_handle);
        }

        remaining -= ret;
        free(string);
        cJSON_Delete(root);
    }
    
    // End response
    ESP_LOGI(TAG, "End response");
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static const httpd_uri_t setdeviceid = {
    .uri = "/setdeviceid",
    .method = HTTP_POST,
    .handler = setdeviceid_get_handler
};

static httpd_handle_t _start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        // Set URI handlers
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &servePage);
        httpd_register_uri_handler(server, &CaptivePortal);
        httpd_register_uri_handler(server, &wifi1_svg);
        httpd_register_uri_handler(server, &wifi2_svg);
        httpd_register_uri_handler(server, &wifi3_svg);
        httpd_register_uri_handler(server, &wififull_svg);
        httpd_register_uri_handler(server, &wifiinfo);
        httpd_register_uri_handler(server, &scanwifi);
        httpd_register_uri_handler(server, &setdeviceid);
        
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

void stop_webserver(void)
{
    ESP_LOGI(TAG, "Stopping webserver");
    // Stop the httpd server
    httpd_stop(server);
}

// static void disconnect_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
// {
//     httpd_handle_t* server = (httpd_handle_t*) arg;
//     if (*server) {
//         ESP_LOGI(TAG, "Stopping webserver");
//         stop_webserver(*server);
//         *server = NULL;
//     }
// }

// static void connect_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
// {
//     httpd_handle_t* server = (httpd_handle_t*) arg;
//     if (*server == NULL) {
//         ESP_LOGI(TAG, "Starting webserver");
//         *server = _start_webserver();
//     }
// }

static void http_server_netconn_serve(struct netconn *conn)
{
  struct netbuf *inbuf;
  char *buf;
  u16_t buflen;
  err_t err;
 
  /* Read the data from the port, blocking if nothing yet there.
   We assume the request (the part we care about) is in one netbuf */
  err = netconn_recv(conn, &inbuf);

  if (err == ERR_OK) {
    netbuf_data(inbuf, (void**)&buf, &buflen);

    // strncpy(_mBuffer, buf, buflen);

    /* Is this an HTTP GET command? (only check the first 5 chars, since
    there are other formats for GET, and we're keeping it very simple )*/
    printf("buffer = %s \n", buf);
    if (buflen>=5 &&
        buf[0]=='G' &&
        buf[1]=='E' &&
        buf[2]=='T' &&
        buf[3]==' ' &&
        buf[4]=='/' ) {
          printf("buf[5] = %c\n", buf[5]);
      /* Send the HTML header
             * subtract 1 from the size, since we dont send the \0 in the string
             * NETCONN_NOCOPY: our data is const static, so no need to copy it
       */

      netconn_write(conn, http_html_hdr, sizeof(http_html_hdr)-1, NETCONN_NOCOPY);
      netconn_write(conn, indexHtmlStart, indexHtmlEnd-indexHtmlStart-1, NETCONN_NOCOPY);
    }

  }
  /* Close the connection (server closes in HTTP) */
  netconn_close(conn);

  /* Delete the buffer (netconn_recv gives us ownership,
   so we have to make sure to deallocate the buffer) */
  netbuf_delete(inbuf);
}

void start_webserver(void) {
    ESP_LOGI(TAG, "Starting webserver");
    connected = false;
    // ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    init_ap_config();
    ESP_ERROR_CHECK(esp_wifi_start());

    captdnsInit();
    if (LWIP) {
        struct netconn *conn, *newconn;
        err_t err;
        conn = netconn_new(NETCONN_TCP);
        netconn_bind(conn, NULL, 80);
        netconn_listen(conn);
        do {
            err = netconn_accept(conn, &newconn);
            if (err == ERR_OK) {
            http_server_netconn_serve(newconn);
            netconn_delete(newconn);
            }
        } while(err == ERR_OK);
        netconn_close(conn);
        netconn_delete(conn);
    } else {
        /* Register event handlers to stop the server when Wi-Fi or Ethernet is disconnected,
        * and re-start it upon connection.
        */
        // ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));
        // ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));
#ifdef CONFIG_EXAMPLE_CONNECT_ETHERNET
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, &disconnect_handler, &server));
#endif // CONFIG_EXAMPLE_CONNECT_ETHERNET

        /* Start the server for the first time */
        server = _start_webserver();
    }
}