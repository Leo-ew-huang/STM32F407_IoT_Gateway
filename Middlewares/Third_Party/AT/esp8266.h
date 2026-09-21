#ifndef ESP8266_H
#define ESP8266_H

#include "at_device.h"


int esp8266_init(const AT_PORT *port);

int esp8266_connect_ap(const char *ssid, const char *passwd);

int esp8266_get_ip(PAT_Device pDev);

#endif /* ESP8266_H */

