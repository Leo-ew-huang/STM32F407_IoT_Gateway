#include "app_test_task.h"
#include "FreeRTOS.h"
#include "task.h"

void app_test(void *argument)
{
    uart2_rx_start(); // 启动 UART2 的 DMA + 空闲中断接收

    while(1)
    {
        uint8_t data;
        if (rb_read(&uart2_ringbuf, &data) == 0) // 从环形缓冲区读取一个字节
            printf("Received: %02X\n", data); // 打印接收到的字节
        vTaskDelay(pdMS_TO_TICKS(100)); // 延时 100 毫秒
    }
}