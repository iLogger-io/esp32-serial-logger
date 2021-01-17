#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_netif.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_websocket_client.h"

#include "driver/uart.h"
#include "driver/gpio.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "cJSON.h"

#include "common_config.h"
#include "local_webserver_apsta.h"

#define BUF_SIZE                2048

#define WEBSOCKET_EVENT_CONNECTED_BIT      BIT0

static const char *TAG = "main";

typedef struct
{
    uint32_t length;
    char *str;
} msg_queue_t;

xQueueHandle uart_queue_hdl, ws_queue_hdl;
SemaphoreHandle_t xSemaphore_app = NULL;
static xQueueHandle gpio_evt_queue = NULL;
static EventGroupHandle_t wss_event_group;
extern volatile char __DEVICEID[];
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
static void uart_read_buffer()
{
    // Configure parameters of an UART driver,
    // communication pins and install the driver
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_param_config(UART_NUM_2, &uart_config);
    uart_driver_install(UART_NUM_2, BUF_SIZE, 0, 0, NULL, 0);
    uart_set_pin(UART_NUM_2, GPIO_TX_IO_PIN, GPIO_RX_IO_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    // Configure a temporary buffer for the incoming data
    uint8_t *data = (uint8_t *) malloc(BUF_SIZE);

    while (true) {
        // Read data from the UART
        int len = uart_read_bytes(UART_NUM_2, data, BUF_SIZE, 100 / portTICK_RATE_MS); //100
        // Write data back to the UART
        if (len > 0) {
            msg_queue_t msg_queue_send;
            char *str = pvPortMalloc(len + 1);
            memcpy(str, data, len);
            str[len] = '\0';
            msg_queue_send.str = str;
            msg_queue_send.length = len;
            if(xQueueSend(uart_queue_hdl, &msg_queue_send, (portTickType) 100) != pdPASS)
            {
                vPortFree(str);
                ESP_LOGI(TAG, "Queue full");
            }
        }
    }
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        xEventGroupSetBits(wss_event_group, WEBSOCKET_EVENT_CONNECTED_BIT);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
        break;
    case WEBSOCKET_EVENT_DATA:
        if(data->op_code == 1) {
            msg_queue_t msg_queue_send;
            char *str = pvPortMalloc(data->data_len + 1);
            memcpy(str, data->data_ptr, data->data_len);
            str[data->data_len] = '\0';
            msg_queue_send.str = str;
            msg_queue_send.length = data->data_len;
            if(xQueueSendFromISR(ws_queue_hdl, &msg_queue_send, NULL) != pdPASS)
            {
                vPortFree(str);
                ESP_LOGI(TAG, "Queue full");
            }
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_ERROR");
        break;
    }
}

static void wifi_init(void *pvParameters)
{
    start_wifi_service();
    
    ESP_LOGI(TAG, "Finish wifi init");
    vTaskDelete(NULL);
}

void read_device_id(void)
{
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
        size_t deviceid_size;
        err = nvs_get_str(nvs_handle, "deviceid", __DEVICEID, &deviceid_size);
        switch (err)
        {
        case ESP_OK:
            __DEVICEID[deviceid_size] = '\0';
            printf("Done\n");
            printf("[%d] __DEVICEID=%s \n", deviceid_size, __DEVICEID);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            printf("The value is not initialized yet!\n");
            break;
        default:
            printf("Error (%s) reading!\n", esp_err_to_name(err));
        }

        // Close
        nvs_close(nvs_handle);
    }
}

static void app(void *pvParameters)
{
    TickType_t xLastWakeTime;
    esp_websocket_client_config_t websocket_cfg = {};
    // websocket_cfg.uri = "wss://api.ilogger.io";
    websocket_cfg.uri = "ws://192.168.1.167:3000";

    uart_queue_hdl = xQueueCreate(20, sizeof(msg_queue_t));
    msg_queue_t msg_queue_receive;
    wss_event_group = xEventGroupCreate();
    
    char send_bufer[BUF_SIZE + 512];
    char json_post[512];

    read_device_id();

    // Initialise the xLastWakeTime variable with the current time.
    xLastWakeTime = xTaskGetTickCount();

    if( xSemaphoreTake( xSemaphore_app, ( TickType_t ) portMAX_DELAY ) == pdTRUE )
    {
        xSemaphoreGive( xSemaphore_app );
    }

    ESP_LOGI(TAG, "app starting");

    ESP_LOGI(TAG, "Connecting to %s...", websocket_cfg.uri);

    esp_websocket_client_handle_t client = esp_websocket_client_init(&websocket_cfg);
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)client);
    esp_websocket_client_start(client);

    while(1) {
        if(xQueueReceive(uart_queue_hdl, &msg_queue_receive, (portTickType) 1000 / portTICK_RATE_MS) == pdPASS)
        {
            ESP_LOGI(TAG, "Receive queue: %d", msg_queue_receive.length);
            char *json_string = NULL;
            cJSON *json_device_data = cJSON_CreateObject();
            cJSON_AddStringToObject(json_device_data, "topic", "device_data");
            cJSON *payload = cJSON_CreateObject();
            cJSON_AddStringToObject(payload, "data", msg_queue_receive.str);
            cJSON_AddItemToObject(json_device_data, "payload", payload);
            json_string = cJSON_Print(json_device_data);
            cJSON_Delete(json_device_data);
            esp_websocket_client_send(client, json_string, strlen(json_string), 20000 / portTICK_PERIOD_MS);
            free(json_string);
            vPortFree(msg_queue_receive.str);
        }

        EventBits_t bits = xEventGroupWaitBits(wss_event_group,
        WEBSOCKET_EVENT_CONNECTED_BIT,
        pdFALSE,
        pdFALSE,
        (portTickType) 1 / portTICK_RATE_MS);
        if (bits & WEBSOCKET_EVENT_CONNECTED_BIT) {
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
            /* 
            {
                topic: 'register_device',
                payload: {
                    deviceid: 'NUx9oC139rnCnLP2D+y85EpDW0PYZBQpORcZ0ZHdQk3aoM0LIM6AGVtJHGmtobmp'
                }
            }
            */
            char *json_string = NULL;
            cJSON *json_register_device = cJSON_CreateObject();
            cJSON_AddStringToObject(json_register_device, "topic", "register_device");
            cJSON *payload = cJSON_CreateObject();
            cJSON_AddStringToObject(payload, "deviceid", __DEVICEID);
            ESP_LOGI(TAG, "deviceid:  %s", __DEVICEID);
            cJSON_AddItemToObject(json_register_device, "payload", payload);
            json_string = cJSON_Print(json_register_device);
            ESP_LOGI(TAG, "%s", json_string);
            cJSON_Delete(json_register_device);
            esp_websocket_client_send(client, json_string, strlen(json_string), 20000 / portTICK_PERIOD_MS);
            free(json_string);
            xEventGroupClearBits(wss_event_group, WEBSOCKET_EVENT_CONNECTED_BIT);
        }
    }

    vTaskDelete(NULL);
}

static void gpio_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

void init_gpio(void) {
    gpio_config_t io_conf;
    //disable interrupt
    io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
    //set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    //bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    //disable pull-down mode
    io_conf.pull_down_en = 0;
    //disable pull-up mode
    io_conf.pull_up_en = 0;
    //configure GPIO with the given settings
    gpio_config(&io_conf);

    //interrupt of rising edge
    io_conf.intr_type = GPIO_PIN_INTR_POSEDGE;
    //bit mask of the pins, use GPIO4/5 here
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    //set as input mode    
    io_conf.mode = GPIO_MODE_INPUT;
    //enable pull-up mode
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

    //change gpio intrrupt type for one pin
    gpio_set_intr_type(GPIO_INPUT_IO_0, GPIO_INTR_ANYEDGE);
}

static void startwebserver(void *pvParameters)
{
    start_webserver();
    vTaskDelete(NULL);
}

static void mainapp(void *pvParameters)
{
    uint32_t press_count = 0;
    msg_queue_t msg_queue_receive;
    ws_queue_hdl = xQueueCreate(10, sizeof(msg_queue_t));
    init_gpio();
    gpio_set_level(GPIO_OUTPUT_RESET, true);
    while(true) {
        if(xQueueReceive(ws_queue_hdl, &msg_queue_receive, (portTickType) 100 / portTICK_RATE_MS) == pdPASS)
        {
            ESP_LOGI(TAG, "Receive ws queue: %d", msg_queue_receive.length);
            ESP_LOGI(TAG, "%d  %s", strlen(msg_queue_receive.str), msg_queue_receive.str);
            cJSON *root = cJSON_Parse((char *)msg_queue_receive.str);
            char *string = cJSON_Print(root);
            ESP_LOGI(TAG, "%s", string);

            json_tmp = cJSON_GetObjectItemCaseSensitive(root, "topic");
            if (strcmp(json_tmp->valuestring, "topic") == 0) {
                ESP_LOGI(TAG, "topic");
                json_tmp = cJSON_GetObjectItemCaseSensitive(root, "type");
                if (strcmp(json_tmp->valuestring, "reset") == 0) {
                    ESP_LOGI(TAG, "reset");
                    gpio_set_level(GPIO_OUTPUT_RESET, false);
                    vTaskDelay(1000 / portTICK_PERIOD_MS);
                    gpio_set_level(GPIO_OUTPUT_RESET, true);
                }
            } else if (strcmp(json_tmp->valuestring, "commandline") == 0) {
                ESP_LOGI(TAG, "commandline");
                json_tmp = cJSON_GetObjectItemCaseSensitive(root, "string");
                ESP_LOGI(TAG, "%s", json_tmp->valuestring);
                uart_write_bytes(UART_NUM_2, (const char *) json_tmp->valuestring, strlen(json_tmp->valuestring));
            }
            free(string);
            cJSON_Delete(root);
            vPortFree(msg_queue_receive.str);
        }
        if (gpio_get_level(GPIO_INPUT_IO_0) == 0) {
            while(gpio_get_level(GPIO_INPUT_IO_0) == 0) {
                press_count++;
                vTaskDelay(100 / portTICK_PERIOD_MS);
            }
            ESP_LOGI(TAG, "count: %d\n", press_count);

            if (press_count > 50) {
                xTaskCreate(startwebserver, "startwebserver", 4096, NULL, 4, NULL);
            }

            press_count = 0;
        }
        
        // vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    vTaskDelete(NULL);
}

void app_main(void)
{
    xSemaphore_app = xSemaphoreCreateBinary();
    ESP_LOGI(TAG, "NVS init");
    ESP_ERROR_CHECK(nvs_flash_init());

    xTaskCreate(wifi_init, "wifi_init", 4096, NULL, 5, NULL);
    xTaskCreate(app, "app", 8192 * 4, NULL, 5, NULL);
    xTaskCreate(mainapp, "mainapp", 2048, NULL, 4, NULL);
    xTaskCreate(uart_read_buffer, "uart_read_buffer", 4096, NULL, 6, NULL);
}