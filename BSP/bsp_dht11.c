#include "bsp_dht11.h"
#include "gpio.h"
#include "FreeRTOS.h"
#include "task.h"
#include "core_cm4.h"
#include <stdio.h>

/**
 * @brief 初始化DWT周期计数器，只调用1次，放在系统时钟初始化完成之后
 */
void DWT_Init(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;  // 使能DWT跟踪模块
  DWT->CYCCNT = 0U;                                 // 周期计数器清零
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;              // 开启CYCCNT计数
}

/**
 * @brief DWT阻塞微秒延时
 * @param us 要延时的微秒数
 * @note 无符号减法天然处理CYCCNT溢出回绕
 */
static inline void delay_us(uint32_t us)
{
  uint32_t start = DWT->CYCCNT;
  uint32_t ticks = us * (SystemCoreClock / 1000000UL);
  while ((DWT->CYCCNT - start) < ticks)
  {
    ;
  }
}

static void dht11_set_output(void)
{
    // PC2 设置为【通用输出】MODER=01
    GPIOC->MODER &= ~GPIO_MODER_MODER2_Msk;   // 先清PC2对应的2bit
    GPIOC->MODER |=  GPIO_MODER_MODER2_0;     // 置01，输出模式
}

static void dht11_set_input(void)
{
    // PC2 设置为【输入】MODER=00
    GPIOC->MODER &= ~GPIO_MODER_MODER2_Msk;
    // 不做 |= 赋值，保持0即可
}


static void dht11_drive_high(void)
{
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_2, GPIO_PIN_SET);
}


static void dht11_drive_low(void)
{
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_2, GPIO_PIN_RESET);
}

static GPIO_PinState dht11_read_pin(void)
{
    return HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_2);
}

int dht11_read(uint8_t *humidity, uint8_t *temperature)
{
    uint8_t buf[5] = {0};
    uint8_t i, j;
    uint32_t t_start, t_now;

    /* DWT 未初始化则 CYCCNT 不计数, 超时/delay_us 会死等。
       DWT_Init 幂等, 重复调用无害(正式调用位置: app_mqtt_task 初始化序列) */
    DWT_Init();

    /* ========== 1. 起始信号(临界区外: 18ms 长等待, 时序不敏感, 让出CPU) ========== */
    dht11_set_output();
    dht11_drive_low();
    vTaskDelay(pdMS_TO_TICKS(20));     /* 拉低 >=18ms */
    dht11_drive_high();                /* 释放总线, 20~40us 后 DHT 应答 */

    /* ========== 2. 应答检测(临界区内: us级时序, 禁止调度) ==========
     * 应答 = 低80us + 高80us。每段"只要还处于X就等", 退出 = 状态翻转;
     * 超时阈值保护的是"电平保持时长 + 抖动余量"(协议80us -> 阈值200us)。
     * 线没接好/传感器坏时靠超时退出而非死循环。 */
    taskENTER_CRITICAL();
    dht11_set_input();
    delay_us(40);                      /* 主机释放 20~40us, 让总线稳定在高位 */

    if (dht11_read_pin() != 0)         /* 释放期结束还不是低 = 模块无应答 */
    {
        taskEXIT_CRITICAL();
        printf("DHT11 no response\r\n");
        return -1;
    }

    /* 等应答低电平结束(80us): "只要还低就等", 退出 = 变高 */
    t_start = DWT->CYCCNT;
    while (dht11_read_pin() == 0)
    {
        t_now = DWT->CYCCNT;
        if ((t_now - t_start) > 200 * (SystemCoreClock / 1000000UL))
        {
            taskEXIT_CRITICAL();
            printf("DHT11 ack low timeout\r\n");
            return -1;
        }
    }

    /* 等应答高电平结束(80us): "只要还高就等", 退出 = 变低(bit0 低电平起点) */
    t_start = DWT->CYCCNT;
    while (dht11_read_pin() != 0)
    {
        t_now = DWT->CYCCNT;
        if ((t_now - t_start) > 200 * (SystemCoreClock / 1000000UL))
        {
            taskEXIT_CRITICAL();
            printf("DHT11 ack high timeout\r\n");
            return -1;
        }
    }

    /* ========== 3. 读 40bit(高位在前): 低50us + 高(26~28us=0 / 70us=1) ========== */
    for (i = 0; i < 5; i++)
    {
        uint8_t val = 0;               /* 每字节从零开始, 防上一字节残留污染 */
        for (j = 0; j < 8; j++)
        {
            /* 等低电平结束(50us): 进入时正站在低电平开头 */
            t_start = DWT->CYCCNT;
            while (dht11_read_pin() == 0)
            {
                t_now = DWT->CYCCNT;
                if ((t_now - t_start) > 150 * (SystemCoreClock / 1000000UL))
                {
                    taskEXIT_CRITICAL();
                    printf("DHT11 bit low timeout\r\n");
                    return -1;
                }
            }

            /* 掐高电平宽度: 起点放"变高"时刻, 终点取"变低"时刻 */
            t_start = DWT->CYCCNT;
            while (dht11_read_pin() != 0)
            {
                t_now = DWT->CYCCNT;
                if ((t_now - t_start) > 150 * (SystemCoreClock / 1000000UL))
                {
                    taskEXIT_CRITICAL();
                    printf("DHT11 bit high timeout\r\n");
                    return -1;
                }
            }
            val <<= 1;                 /* 高位在前: 先移位, 再填最低位 */
            if (((t_now - t_start) / (SystemCoreClock / 1000000UL)) > 40)
                val |= 0x01;
        }
        buf[i] = val;
    }

    taskEXIT_CRITICAL();

    /* ========== 4. 校验: 和的低8位(uint8_t相加会提升成int, 必须显式&0xFF;
                运算符优先级 != 高于 &, 外层括号不能省) ========== */
    if (((buf[0] + buf[1] + buf[2] + buf[3]) & 0xFF) != buf[4])
    {
        printf("DHT11 checksum error: %u %u %u %u %u\r\n",
               buf[0], buf[1], buf[2], buf[3], buf[4]);
        return -1;
    }

    /* ========== 5. 回传(DHT11 小数字节恒0, 直接取整数字节) ========== */
    *humidity    = buf[0];
    *temperature = buf[2];
    return 0;
}


