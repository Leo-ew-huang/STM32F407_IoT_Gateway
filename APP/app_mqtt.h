#ifndef APP_MQTT_H
#define APP_MQTT_H

typedef struct dev_cmd_t
{
    const char *dev;
    void (*on)(void);
    void (*off)(void);
} dev_cmd_t;


int transport_sendPacketBuffer(int sock, unsigned char *buf, int len);

int transport_getdata(unsigned char *buf, int count);

int transport_close(int sock);

void app_mqtt_task(void *argument);


#endif /* APP_MQTT_H */
