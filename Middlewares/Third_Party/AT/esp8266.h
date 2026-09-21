#ifndef ESP8266_H
#define ESP8266_H

#include "at_device.h"


int esp8266_init(const AT_PORT *port);

int esp8266_connect_ap(const char *ssid, const char *passwd);

int esp8266_get_ip(PAT_Device pDev);

int esp8266_tcp_connect(const char *host, uint16_t port);
/* AT+CIPMUX=0 → AT+CIPSTART="TCP","host",port → 应答 CONNECT+OK */

int esp8266_send(const uint8_t *data, uint16_t len);

int esp8266_recv(uint8_t *buf, uint16_t len, uint32_t timeout_ms);

#endif /* ESP8266_H */

