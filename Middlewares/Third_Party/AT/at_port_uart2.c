#include "at_port_uart2.h"
#include "uart2_driver.h"

static int stm32_uart_init(void)
{
    uart2_rx_start();
    return 0;
}

static int stm32_uart_send(const uint8_t *data, uint16_t len)
{
    return uart2_send(data, len);
}

static int stm32_uart_recv(uint8_t *byte, uint32_t timeout_ms)
{
    return uart2_receive_blocking(byte, timeout_ms);
}

const AT_PORT at_port_stm32_uart2 = 
{
    stm32_uart_init,
    stm32_uart_send,
    stm32_uart_recv,
};
