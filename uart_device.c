#include "stm32f4xx_hal.h"
#include "stm32f4xx_hal_uart.h"
#include "uart_device.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "queue.h"
#include "string.h"

/* 接收队列与DMA缓冲必须能容纳浏览器一次完整请求(请求头+body约300~500B),
 * 否则 +IPD 数据入队时队列满会静默丢字节, 请求被截断导致"操作失败";
 * 且保存配置时 Flash 擦除会阻塞 MCU 1~2s, DMA 缓冲够大才能兜住期间到达的数据。
 * 原 100B 太小, 是按钮/保存高概率失败的根因之一。 */
#define UART_RX_QUEUE_LEN  1024
#define UART_RX_BUF_SIZE   1024

extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart2;

static struct UART_Device *g_cur_uart1_dev;
static struct UART_Device *g_cur_uart2_dev;

/* UART2(ESP8266)通信统计, 用于调试 */
volatile uint32_t g_uart2_rx_bytes = 0;
volatile uint32_t g_uart2_tx_ok = 0;
volatile uint32_t g_uart2_tx_fail = 0;
volatile uint32_t g_uart2_err_cnt = 0;


/******************************************************/
/* 使用中断+FreeRTOS队列,信号量 */


struct UART_Device g_stm32_uart1;

struct UART_Data {
    UART_HandleTypeDef *handle;
    SemaphoreHandle_t xTxSem;
    QueueHandle_t xRxQueue;
    uint8_t rxdatas[UART_RX_BUF_SIZE];
};


void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    struct UART_Data *data;
    
    if (huart == &huart1 && g_cur_uart1_dev)
    {
        data = g_cur_uart1_dev->priv_data;
        
        /* 释放信号量 */
        xSemaphoreGiveFromISR(data->xTxSem, NULL);
    }
    else if (huart == &huart2 && g_cur_uart2_dev)
    {
        data = g_cur_uart2_dev->priv_data;
        
        /* 释放信号量 */
        xSemaphoreGiveFromISR(data->xTxSem, NULL);
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    struct UART_Data *data;
    int len = huart->RxXferSize - huart->RxXferCount;
    
    if (huart == &huart1 && g_cur_uart1_dev)
    {
        data = g_cur_uart1_dev->priv_data;
        
        /* 写队列 */
        for (int i = 0; i < len; i++)
        {
            xQueueSendFromISR(data->xRxQueue, &data->rxdatas[i], NULL);
        }

        /* 再次启动数据的接收 */
        if (g_cur_uart1_dev == &g_stm32_uart1)
            HAL_UART_Receive_IT(data->handle, data->rxdatas, huart->RxXferSize);
        else
            HAL_UARTEx_ReceiveToIdle_DMA(data->handle, data->rxdatas, huart->RxXferSize);
    }
    else if (huart == &huart2 && g_cur_uart2_dev)
    {
        data = g_cur_uart2_dev->priv_data;

        g_uart2_rx_bytes += len;

        /* 写队列 */
        for (int i = 0; i < len; i++)
        {
            xQueueSendFromISR(data->xRxQueue, &data->rxdatas[i], NULL);
        }

        /* 再次启动数据的接收 */
        HAL_UARTEx_ReceiveToIdle_DMA(data->handle, data->rxdatas, huart->RxXferSize);
    }
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    struct UART_Data *data;
    int len = Size;
    
    if (huart == &huart1 && g_cur_uart1_dev)
    {
        data = g_cur_uart1_dev->priv_data;
        
        /* 写队列 */
        for (int i = 0; i < len; i++)
        {
            xQueueSendFromISR(data->xRxQueue, &data->rxdatas[i], NULL);
        }

        /* 再次启动数据的接收 */
        HAL_UARTEx_ReceiveToIdle_DMA(data->handle, data->rxdatas, huart->RxXferSize);
    }
    else if (huart == &huart2 && g_cur_uart2_dev)
    {
        data = g_cur_uart2_dev->priv_data;

        g_uart2_rx_bytes += len;

        /* 写队列 */
        for (int i = 0; i < len; i++)
        {
            xQueueSendFromISR(data->xRxQueue, &data->rxdatas[i], NULL);
        }

        /* 再次启动数据的接收 */
        HAL_UARTEx_ReceiveToIdle_DMA(data->handle, data->rxdatas, huart->RxXferSize);
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    /* 一旦UART发生错误(如overrun), HAL会中止DMA接收且不会自动恢复.
       这里必须重新启动接收, 否则UART2会永久"失聪" */
    if (huart == &huart2 && g_cur_uart2_dev)
    {
        struct UART_Data *data = g_cur_uart2_dev->priv_data;

        g_uart2_err_cnt++;
        __HAL_UART_CLEAR_FLAG(huart, UART_FLAG_ORE | UART_FLAG_FE | UART_FLAG_NE | UART_FLAG_PE);
        HAL_UARTEx_ReceiveToIdle_DMA(data->handle, data->rxdatas, UART_RX_BUF_SIZE);
    }
    else if (huart == &huart1 && g_cur_uart1_dev)
    {
        struct UART_Data *data = g_cur_uart1_dev->priv_data;

        __HAL_UART_CLEAR_FLAG(huart, UART_FLAG_ORE | UART_FLAG_FE | UART_FLAG_NE | UART_FLAG_PE);
        if (g_cur_uart1_dev == &g_stm32_uart1)
            HAL_UART_Receive_IT(data->handle, data->rxdatas, 1);
        else
            HAL_UARTEx_ReceiveToIdle_DMA(data->handle, data->rxdatas, UART_RX_BUF_SIZE);
    }
}



static int stm32_uart_init(struct UART_Device *pDev, int baud, int datas, char parity, int stop)
{
    struct UART_Data *data = pDev->priv_data;

    g_cur_uart1_dev = pDev;

    data->xTxSem = xSemaphoreCreateBinary();
    data->xRxQueue = xQueueCreate(UART_RX_QUEUE_LEN, 1);

    /* 启动第1次数据的接收 */
    HAL_UART_Receive_IT(data->handle, data->rxdatas, 1);
    
    return 0;
}

static int stm32_uart_send(struct UART_Device *pDev, uint8_t *datas, int len, int timeout_ms)
{
    struct UART_Data *data = pDev->priv_data;
    
    /* 仅仅是触发中断而已 */
    HAL_UART_Transmit_IT(data->handle, datas, len);

    /* 等待发送完毕:等待信号量 */
    if (pdTRUE == xSemaphoreTake(data->xTxSem, timeout_ms))
        return 0;
    else
        return -1;
}

static int stm32_uart_recv(struct UART_Device *pDev, uint8_t *data, int timeout_ms)
{
    struct UART_Data *uart_data = pDev->priv_data;
    
    /* 读取队列得到数据, 问题:谁写队列?中断:写队列 */
    if (pdPASS == xQueueReceive(uart_data->xRxQueue, data,timeout_ms))
        return 0;
    else
        return -1;
}

static struct UART_Data g_stm32_uart1_data = {
    &huart1,
};

static struct UART_Device g_stm32_uart1 = {
    "stm32_uart1",
    stm32_uart_init,
    stm32_uart_send,
    stm32_uart_recv,
    &g_stm32_uart1_data,
};


/******************************************************/
/* 使用查询方式,适合裸机 */

static int stm32_bear_uart_init(struct UART_Device *pDev, int baud, int datas, char parity, int stop)
{    
    g_cur_uart1_dev = pDev;
    return 0;
}

static int stm32_bear_uart_send(struct UART_Device *pDev, uint8_t *datas, int len, int timeout_ms)
{
    return HAL_UART_Transmit((UART_HandleTypeDef *)pDev->priv_data, datas, len, timeout_ms);
}

static int stm32_bear_uart_recv(struct UART_Device *pDev, uint8_t *data, int timeout_ms)
{
   return HAL_UART_Receive((UART_HandleTypeDef *)pDev->priv_data, data, 1, timeout_ms);
}



static struct UART_Device g_stm32_bear_uart1 = {
    "stm32_bear_uart1",
    stm32_bear_uart_init,
    stm32_bear_uart_send,
    stm32_bear_uart_recv,
    &huart1,
};



/******************************************************/
/* 使用DMA, FreeRTOS队列,信号量 */

static int stm32_dma_uart_init(struct UART_Device *pDev, int baud, int datas, char parity, int stop)
{
    struct UART_Data *data = pDev->priv_data;

    if (data->handle == &huart2)
        g_cur_uart2_dev = pDev;
    else
        g_cur_uart1_dev = pDev;

    data->xTxSem = xSemaphoreCreateBinary();
    data->xRxQueue = xQueueCreate(UART_RX_QUEUE_LEN, 1);

    /* 启动第1次数据的接收 */
    //HAL_UART_Receive_DMA(data->handle, &data->rxdatas, UART_RX_BUF_SIZE);
    HAL_UARTEx_ReceiveToIdle_DMA(data->handle, data->rxdatas, UART_RX_BUF_SIZE);
    
    return 0;
}

static int stm32_dma_uart_send(struct UART_Device *pDev, uint8_t *datas, int len, int timeout_ms)
{
    struct UART_Data *data = pDev->priv_data;
    int ret;
    
    /* 仅仅是触发中断而已 */
    HAL_UART_Transmit_DMA(data->handle, datas, len);

    /* 等待发送完毕:等待信号量 */
    if (pdTRUE == xSemaphoreTake(data->xTxSem, timeout_ms))
        ret = 0;
    else
        ret = -1;

    if (data->handle == &huart2)
    {
        if (ret == 0) g_uart2_tx_ok++;
        else          g_uart2_tx_fail++;
    }

    return ret;
}

static int stm32_dma_uart_recv(struct UART_Device *pDev, uint8_t *data, int timeout_ms)
{
    struct UART_Data *uart_data = pDev->priv_data;
    
    /* 读取队列得到数据, 问题:谁写队列?中断:写队列 */
    if (pdPASS == xQueueReceive(uart_data->xRxQueue, data,timeout_ms))
        return 0;
    else
        return -1;
}

static struct UART_Data g_stm32_dma_uart1_data = {
    &huart1,
};


static struct UART_Device g_stm32_dma_uart1 = {
    "stm32_dma_uart1",
    stm32_dma_uart_init,
    stm32_dma_uart_send,
    stm32_dma_uart_recv,
    &g_stm32_dma_uart1_data,
};


/******************************************************/
/* 使用DMA, FreeRTOS队列,信号量 (UART2, 连接ESP8266) */

static struct UART_Data g_stm32_dma_uart2_data = {
    &huart2,
};

static struct UART_Device g_stm32_dma_uart2 = {
    "stm32_dma_uart2",
    stm32_dma_uart_init,
    stm32_dma_uart_send,
    stm32_dma_uart_recv,
    &g_stm32_dma_uart2_data,
};


/******************************************************/


struct UART_Device *g_uart_devs[] = {&g_stm32_uart1, &g_stm32_bear_uart1, &g_stm32_dma_uart1, &g_stm32_dma_uart2};

struct UART_Device *GetUARTDevice(char *name)
{
    int i = 0;
    for (i = 0; i < sizeof(g_uart_devs)/sizeof(g_uart_devs[0]); i++)
    {
        if (0 == strcmp(name, g_uart_devs[i]->name))
            return g_uart_devs[i];
    }

    return NULL;
}


