#ifndef UART2_DRIVER_H
#define UART2_DRIVER_H

#include "usart.h"          /* huart2、hdma_usart2_rx */
#include "uart_ringbuf.h"

/*
 * 上层(APP)读取 UART2 收到的原始字节流用。
 * 解析任务通过 rb_read(&uart2_ringbuf, ...) 取出字节。
 */
extern struct RingBuffer uart2_ringbuf;

extern volatile uint32_t g_uart2_rx_bytes;
extern volatile uint32_t g_uart2_tx_ok;
extern volatile uint32_t g_uart2_tx_fail;
extern volatile uint32_t g_uart2_err_cnt;



/*
 * 启动 UART2(ESP8266) 接收：
 * 配置一次 DMA 空闲中断接收，之后 DMA 自动循环接收，
 * HAL_UARTEx_RxEventCallback 在收到一帧(空闲)时被触发。
 */
void uart2_rx_start(void);

int uart2_send(const uint8_t *data, uint16_t len);
int uart2_receive_blocking(uint8_t *byte, uint32_t timeout_ms);


#endif /* UART2_DRIVER_H */
