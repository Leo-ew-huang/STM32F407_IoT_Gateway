#include "uart2_driver.h"
#include "FreeRTOS.h"
#include "task.h"      /* xTaskGetTickCount */
#include "semphr.h"    /* 信号量 */

struct RingBuffer uart2_ringbuf;    /* ← 定义：变量本体，在这里分配内存 */


/* UART2(ESP8266)通信统计, 用于调试 */
volatile uint32_t g_uart2_rx_bytes = 0;
volatile uint32_t g_uart2_tx_ok = 0;
volatile uint32_t g_uart2_tx_fail = 0;
volatile uint32_t g_uart2_err_cnt = 0;

static SemaphoreHandle_t uart2_rx_sem;

/*
 * DMA 直接写入的原始暂存数组（暂存区）。
 *
 * 为什么不直接让 DMA 写进 RingBuffer？
 *  (1) DMA 是盲写：它循环填充、写满 256 就绕回覆盖，不懂 RingBuffer 的
 *     "预留一个空位"判满规则，会覆盖还没被读走的数据 -> 静默丢数据。
 *  (2) RingBuffer 由 APP 任务(慢)读取，若 DMA(快、中断时)也直接操作它，
 *     两个角色会互相踩(竞争)。中间隔一层，让"ISR 回调"成为唯一写者、
 *     "任务"成为唯一读者，读写角色彻底隔离。
 *  (3) DMA 需要一段"固定起始地址、连续写到底"的区；RingBuffer 的可用区间
 *     会随任务读取(tail 移动)变得不连续，DMA 追不上。
 *
 * 结论：uart2_rx_buf 负责"硬件暂存"，RingBuffer 负责"软件储备"，
 *       ISR 回调负责在两者之间搬运。所以必须有个裸数组做过渡。
 */
static uint8_t uart2_rx_buf[RING_BUFFER_SIZE];

/*
 * 记录"上一次已经处理到 uart2_rx_buf 的哪个位置"。
 * 下一次回调就从这个位置继续，把新到的字节增量搬进 RingBuffer，
 * 从而不漏、不重。
 */
static uint16_t last_pos = 0;

/**
 * @brief 启动 UART2 的 DMA + 空闲中断接收。
 * @note  DMA 已配置为 CIRCULAR，只需启动一次，之后自动循环接收。
 */
void uart2_rx_start(void)
{
    if (uart2_rx_sem == NULL ) /* 只建一次 */
        uart2_rx_sem = xSemaphoreCreateBinary();

    /* 复位接收状态：RingBuffer 头尾清零 + 记录位置清零。
       两者必须成对复位，语义才一致；这样重新调用本函数可干净重启接收。 */
    rb_init(&uart2_ringbuf);
    last_pos = 0;

    /* 开启"空闲中断 + DMA 接收"：收到一帧(总线空闲)时触发回调 */
    HAL_UARTEx_ReceiveToIdle_DMA(&huart2, uart2_rx_buf, RING_BUFFER_SIZE);
}

int uart2_send(const uint8_t *data, uint16_t len)
{
    if(HAL_UART_Transmit(&huart2, (uint8_t *)data, len, 1000) == HAL_OK)
    {
        g_uart2_tx_ok++;
        return 0; 
    }
    else
    {
        g_uart2_tx_fail++;
        return -1;
    }
}

int uart2_receive_blocking(uint8_t *byte, uint32_t timeout_ms)
{
    TickType_t t0 = xTaskGetTickCount();
   while(1)
   {
        if(rb_read(&uart2_ringbuf, byte) == 0)
            return 0;
        else if(xTaskGetTickCount() - t0 > pdMS_TO_TICKS(timeout_ms))
            return -1;
        xSemaphoreTake(uart2_rx_sem, pdMS_TO_TICKS(timeout_ms)-((xTaskGetTickCount() - t0)));
   }
}
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if(huart != &huart2)
        return;
    g_uart2_err_cnt++;
    __HAL_UART_CLEAR_FLAG(huart, UART_FLAG_ORE | UART_FLAG_FE | UART_FLAG_NE | UART_FLAG_PE);
    last_pos = 0;
    HAL_UARTEx_ReceiveToIdle_DMA(&huart2, uart2_rx_buf, RING_BUFFER_SIZE);
}



/**
 * @brief UART 接收事件回调（HAL 自动触发，运行在中断上下文）。
 * @param huart 触发事件的串口句柄
 * @param Size  参考字节数（CIRCULAR 模式下不代表"新增字节数"，故这里不用）
 *
 * 思路：用 DMA 计数器算出"DMA 此刻写到数组的哪个位置 head"，
 *       把上次记录位置 last_pos 到 head 之间的新字节搬进 RingBuffer。
 * 注意：这是中断上下文，只做快速搬运；耗时的解析/逻辑留到 APP 任务。
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    (void)Size; /* 本工程 CIRcular 模式下不依赖此参数，避免未使用告警 */
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (huart != &huart2)
    {
        return; /* 只处理 UART2/ESP8266 */
    }

    /* DMA 当前写到的位置 = 缓冲区大小 - 剩余未写字节数，再对大小取模(处理回绕) */
    uint16_t head = (uint16_t)(RING_BUFFER_SIZE - __HAL_DMA_GET_COUNTER(&hdma_usart2_rx)) % RING_BUFFER_SIZE;

    /* 把 last_pos 到 head 之间的新字节逐一带入 RingBuffer */
    while (last_pos != head)
    {
        /* 若缓冲区已满则丢弃新数据并退出，避免在中断里死循环 */
        if (rb_write(&uart2_ringbuf, uart2_rx_buf[last_pos]) == 0)
            g_uart2_rx_bytes++;
        else
            break;
        last_pos = (last_pos + 1) % RING_BUFFER_SIZE;
    }

    /*
     * 同步（不能删，为"缓冲满 -> break"这条异常路径兜底）：
     *  - 正常路径：while 跑到 last_pos==head 才退出，此时 last_pos 本就等于 head，此行冗余；
     *  - 满时丢弃：while 提前 break，last_pos 还没追上 head（中间的字节被丢弃）。
     *    若不做此行，下次回调会反复重试那些旧字节且永远补不回，白绕一圈。
     *    此行显式把被丢弃的旧数据"标记为已处理"，下次直接从新位置继续。
     */
    last_pos = head;

    xSemaphoreGiveFromISR(uart2_rx_sem, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
