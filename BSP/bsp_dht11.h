#ifndef BSP_DHT11_H
#define BSP_DHT11_H

#include <stdint.h>

#define DHT11_PORT   GPIOC
#define DHT11_PIN    GPIO_PIN_2

int dht11_read(uint8_t *humidity, uint8_t *temperature);

#endif /* BSP_DHT11_H */


