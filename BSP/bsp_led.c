#include "bsp_led.h"
#include "gpio.h"

void led_on(void)
{
   HAL_GPIO_WritePin(GPIOF, GPIO_PIN_9|GPIO_PIN_10, GPIO_PIN_RESET);
}

void led_off(void)
{
   HAL_GPIO_WritePin(GPIOF, GPIO_PIN_9|GPIO_PIN_10, GPIO_PIN_SET);
}
