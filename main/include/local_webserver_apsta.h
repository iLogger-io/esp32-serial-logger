#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define LWIP 0

/* IO PIN */
#define GPIO_OUTPUT_RESET       4
#define GPIO_OUTPUT_PIN_SEL     (1ULL<<GPIO_OUTPUT_RESET)
#define GPIO_INPUT_IO_0         0
#define GPIO_INPUT_PIN_SEL      (1ULL<<GPIO_INPUT_IO_0)
#define GPIO_TX_IO_PIN          17
#define GPIO_RX_IO_PIN          16

void start_wifi_service(void);
void start_webserver(void);
void stop_webserver(void);

#ifdef __cplusplus
}
#endif