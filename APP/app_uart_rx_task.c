#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "cmsis_os.h"

#include "uart_task.h"        /* uart2_rx_start, uart2_ringbuf */
#include "uart_ringbuf.h"
#include "uart_protocol.h"

/* 业务队列（在 freertos.c 中定义） */
QueueHandle_t  uart_frame_queue;

/**
 * @brief UART 接收任务(生产者)：
 *        不断地从 RingBuffer 取一个字节喂给协议状态机；
 *        凑齐一整帧就用 osMessageQueuePut 交给业务队列。
 */
void app_uart_rx_task(void *argument)
{
    ProtoParser parser = {0};
    parser.state = ST_WAIT_HEAD;

    uart2_rx_start();   /* 启动 UART2 的 DMA 空闲接收 */

    while(1)
    {
        uint8_t byte;
        if (rb_read(&uart2_ringbuf, &byte) == 0) /* 读到 1 字节 */
        {
            if (proto_feed(&parser, byte) == 1)  /* 凑齐一帧 */
            {
                /* 把帧拷进队列（消息队列是值拷贝），交给业务任务 */
                xQueueSend(uart_frame_queue, &parser.frame, 0);
                /* proto_feed 凑齐帧时已把 state 复位为 WAIT_HEAD，无需再手动复位 */
            }
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(100)); /* 缓冲空，让出 CPU 一会儿 */
        }
    }
}