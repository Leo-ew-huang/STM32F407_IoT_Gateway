#ifndef APP_MQTT_H
#define APP_MQTT_H

int transport_sendPacketBuffer(int sock, unsigned char *buf, int len);

int transport_getdata(unsigned char *buf, int count);

int transport_close(int sock);

void app_mqtt_task(void *argument);




#endif /* APP_MQTT_H */
