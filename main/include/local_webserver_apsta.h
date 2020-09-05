#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* */
#define LWIP 0

void start_wifi_service(void);
void start_webserver(void);
void stop_webserver(void);

#ifdef __cplusplus
}
#endif