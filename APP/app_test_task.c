#include "app_test_task.h"

#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"
#include "uart_protocol.h"   /* ProtoFrame、CMD_* 枚举 */
#include "uart_task.h"       /* g_uart2_* 统计计数器 */

/*
 * 业务队列（由 freertos.c 创建，UART 生产者任务往里放完整帧，
 * 本任务作为消费者从队列取帧处理）。
 */
extern osMessageQueueId_t uart_frame_queue;

/* ======================================================================
 * 协议数据格式契约表（收发两端务必一致；按需修改）
 * ======================================================================
 * CMD                 | 含义     | DATA 长度 | 数据格式说明
 * --------------------+----------+-----------+------------------------------
 * CMD_TEMPERATURE(01) | 温度上报 | 2 字节    | 大端，单位 0.1℃（0x0109=265=26.5）
 * CMD_HUMIDITY   (02) | 湿度上报 | 2 字节    | 大端，单位 0.1%RH
 * CMD_LIGHT      (03) | 光照上报 | 4 字节    | 大端 uint32，单位 Lx
 * CMD_LED_CTRL   (04) | LED 控制 | 1 字节    | 0=关 1=开
 * CMD_BUZZER     (05) | 蜂鸣器控 | 1 字节    | 0=关 1=开
 * CMD_GET_STATUS (06) | 查状态   | 0 字节    | 无数据
 * CMD_SET_PERIOD (07) | 设周期   | 2 字节    | 大端，单位 ms
 * ====================================================================== */

/* 把 2 字节大端数据拼成 uint16_t */
static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)(p[0] << 8) | p[1]);
}

/* 把 4 字节大端数据拼成 uint32_t */
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

void app_test(void *argument)
{
    ProtoFrame rx;   /* 每次从队列取出的完整帧 */

    for (;;)
    {
        /* 阻塞取帧：队列没有帧就一直等，让出 CPU；取到才往下走 */
        if (osMessageQueueGet(uart_frame_queue, &rx, NULL, osWaitForever) != osOK)
            continue;

        /*
         * 逐命令处理。
         * 每支先校验 rx.len 是否符合契约，不符则报错跳过，避免用脏数据。
         */
        switch (rx.cmd)
        {
        case CMD_TEMPERATURE:
            if (rx.len == 2)
            {
                uint16_t t = be16(rx.data);         /* 0x0109 -> 265 */
                printf("温度: %u.%u °C\r\n", t / 10, t % 10);  /* 26.5 */
            }
            else
                printf("temp len err=%u\r\n", rx.len);
            break;

        case CMD_HUMIDITY:
            if (rx.len == 2)
            {
                uint16_t h = be16(rx.data);
                printf("湿度: %u.%u %%RH\r\n", h / 10, h % 10);
            }
            else
                printf("humi len err=%u\r\n", rx.len);
            break;

        case CMD_LIGHT:
            if (rx.len == 4)
                printf("光照: %lu Lx\r\n", (unsigned long)be32(rx.data));
            else
                printf("light len err=%u\r\n", rx.len);
            break;

        case CMD_LED_CTRL:
            if (rx.len == 1)
                printf("LED: %s\r\n", rx.data[0] ? "ON" : "OFF");
            else
                printf("led len err=%u\r\n", rx.len);
            break;

        case CMD_BUZZER:
            if (rx.len == 1)
                printf("蜂鸣器: %s\r\n", rx.data[0] ? "ON" : "OFF");
            else
                printf("buzzer len err=%u\r\n", rx.len);
            break;

        case CMD_GET_STATUS:
            printf("收到: 查询设备状态\r\n");
            break;

        case CMD_SET_PERIOD:
            if (rx.len == 2)
                printf("采样周期: %u ms\r\n", be16(rx.data));
            else
                printf("period len err=%u\r\n", rx.len);
            break;

        default:
            /* 未知命令，打印原始帧便于排查 */
            printf("未知 cmd=0x%02X len=%u data=", rx.cmd, rx.len);
            for (uint8_t i = 0; i < rx.len; i++)
                printf("%02X ", rx.data[i]);
            printf("\r\n");
            break;
        }
    }
}

