#ifndef UART_TASK_H
#define UART_TASK_H

#include "usart.h"          /* huart2、hdma_usart2_rx */
#include "uart_ringbuf.h"

extern volatile uint32_t g_uart2_rx_bytes = 0;
extern volatile uint32_t g_uart2_tx_ok = 0;
extern volatile uint32_t g_uart2_tx_fail = 0;
extern volatile uint32_t g_uart2_err_cnt = 0;



/*
 * 启动 UART2(ESP8266) 接收：
 * 配置一次 DMA 空闲中断接收，之后 DMA 自动循环接收，
 * HAL_UARTEx_RxEventCallback 在收到一帧(空闲)时被触发。
 */
void uart2_rx_start(void);

/*
 * 上层(APP)读取 UART2 收到的原始字节流用。
 * 解析任务通过 rb_read(&uart2_ringbuf, ...) 取出字节。
 */
extern RingBuffer uart2_ringbuf;

#endif /* UART_TASK_H */