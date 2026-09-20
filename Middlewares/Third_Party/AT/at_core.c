/*
 * at_core.c —— AT 协议栈 core 层（一问一答引擎）
 *
 * 职责:
 *   1. at_exec_cmd  : 发命令 → 等"应答结束"信号量 → 返回结论
 *   2. at_parse_task: 常驻后台, 把字节流攒成行, 识别应答结束标志(OK/ERROR/FAIL/'>'提示符),
 *                     并跳过与刚发命令相同的回显行(回显免疫)
 *   3. at_init      : 注入 port 适配表 + 创建同步对象 + 启动解析任务(只建资源, 不做 I/O)
 *
 * 铁律: 本文件禁止 include 任何具体硬件驱动头文件(uart2_driver.h 等),
 *       收发只走 dev->port-> 函数指针 —— 换硬件 = 重写 port 层, 本文件一行不改。
 *       同理 core 不认识任何具体 AT 命令: 探活("AT")/关回显("ATE0")等
 *       命令序列全部归 module 层, core 靠"回显免疫"自保而非依赖 ATE0。
 */
#include "at_device.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "cmsis_os.h"
#include <string.h>

/* 单例: 本工程只有一个 WIFI 模块, 不做设备注册表 */
static AT_Device g_at_dev;

/* 攒行缓冲: AT 应答单行远达不到 128 */
#define AT_LINE_BUF_LEN  128

/*=====================================================================
 * 把一行应答拼接进完整应答缓冲(带截断保护)
 * 格式: <line>\r\n —— 与 ESP8266 原始应答保持一致, 方便上层原样查看
 *=====================================================================*/
static void resp_append(PAT_Device dev, const uint8_t *line, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++)
    {
        /* 预留 \r \n \0 三个字节, 防止越界 */
        if (dev->resp_len >= AT_RESP_BUF_LEN - 3)
            break;
        dev->resp_buf[dev->resp_len++] = line[i];
    }
    dev->resp_buf[dev->resp_len++] = '\r';
    dev->resp_buf[dev->resp_len++] = '\n';
    dev->resp_buf[dev->resp_len]   = '\0';
}

/*=====================================================================
 * 解析任务: 字节流 → 行 → 识别结束标志 → 按门铃(at_sem)
 *
 * 它是 port->recv_byte 的唯一长期消费者; at_exec_cmd 在 at_sem 上睡觉,
 * 由本任务在见到结束标志时唤醒 —— 两级"门铃"各守一层, 互不混用。
 *=====================================================================*/
static void at_parse_task(void *arg)
{
    PAT_Device dev = (PAT_Device)arg;
    uint8_t  byte;
    uint8_t  line[AT_LINE_BUF_LEN];
    uint32_t len = 0;

    for (;;)
    {
        /* 阻塞收 1 字节; 200ms 没数据就转回去继续等, 攒了一半的行保留 */
        if (dev->port->recv_byte(&byte, 200) != 0)
            continue;

        /* '>' 提示符: AT+CIPSEND 后模块用它表示"可以发数据了"。
         * 它单独出现、没有换行, 必须在攒行逻辑之前处理 */
        if (byte == '>')
        {
            dev->resp_status = AT_RESP_OK;
            xSemaphoreGive(dev->at_sem);
            len = 0;
            continue;
        }

        /* '\r' 只是回车, 统一以 '\n' 作为行尾 */
        if (byte == '\r')
            continue;

        if (byte == '\n')
        {
            /* ★ 截断残留: len=0 只表示"本行没攒到字节", line[] 里还躺着
             * 上一行的旧字节。不补 '\0' 的话, strstr 会对空行误匹配出
             * 上一行的 "OK" → 门铃提前响 → 应答错位。C 经典三连坑:
             * 缓冲区复用 + 未终止字符串 + strlen/strstr 类函数 */
            line[len] = '\0';

            /* 回显免疫: 与刚发出的命令一字不差的行 = 模块回显, 整行跳过
             * (不进 resp、不参与 OK/ERROR 判定)。无论回显开/关/被 RST 复活,
             * 命令文本里的 OK/ERROR 子串(如 ssid 带"OK")都不会误触发门铃 */
            if (len > 0 && dev->last_cmd_len > 0 &&
                len == dev->last_cmd_len &&
                memcmp(line, dev->last_cmd, len) == 0)
            {
                len = 0;
                continue;
            }

            /* 行结束: 先拼进完整应答, 再判断是不是命令结束标志 */
            resp_append(dev, line, len);

            if (strstr((char *)line, "ERROR") != NULL ||
                strstr((char *)line, "FAIL")  != NULL)
            {
                dev->resp_status = AT_RESP_ERROR;
                xSemaphoreGive(dev->at_sem);
            }
            else if (strstr((char *)line, "OK") != NULL)
            {
                dev->resp_status = AT_RESP_OK;
                xSemaphoreGive(dev->at_sem);
            }

            len = 0;   /* 行复位, 准备攒下一行 */
            continue;
        }

        /* 普通字节: 攒进当前行(超长丢弃防越界, 正常 AT 应答达不到) */
        if (len < AT_LINE_BUF_LEN - 1)
            line[len++] = byte;
    }
}

/*=====================================================================
 * 发送一条 AT 命令并阻塞等结论
 *=====================================================================*/
int at_exec_cmd(const char *cmd, uint32_t timeout_ms)
{
    PAT_Device dev = &g_at_dev;
    char     buf[AT_CMD_BUF_LEN];
    uint32_t cmd_len = strlen(cmd);
    int      ret;

    /* 互斥: 同一时刻只允许一条 AT 对话, 防止两个任务的命令/应答串台 */
    xSemaphoreTake(dev->at_lock, portMAX_DELAY);

    /* 清掉上一次超时后迟到的门铃, 防止旧应答污染本次结论 */
    xSemaphoreTake(dev->at_sem, 0);

    /* 复位应答区: 默认结论 = 超时, 只有解析任务见到结束标志才改写 */
    dev->resp_len    = 0;
    dev->resp_buf[0] = '\0';
    dev->resp_status = AT_RESP_TIMEOUT;

    /* 去掉调用方命令尾部可能自带的 \r\n, 再统一补齐(协议要求 \r\n 结尾) */
    while (cmd_len > 0 && (cmd[cmd_len - 1] == '\r' || cmd[cmd_len - 1] == '\n'))
        cmd_len--;
    if (cmd_len > AT_CMD_BUF_LEN - 3)
        cmd_len = AT_CMD_BUF_LEN - 3;

    /* 记住命令原文(无\r\n), 解析任务靠它识别并跳过回显行。
     * 必须在发送前写入: 回显字节只会在发送之后到达, 到时 last_cmd 必已就位 */
    memcpy(dev->last_cmd, cmd, cmd_len);
    dev->last_cmd[cmd_len] = '\0';
    dev->last_cmd_len = (uint16_t)cmd_len;

    memcpy(buf, cmd, cmd_len);
    buf[cmd_len++] = '\r';
    buf[cmd_len++] = '\n';

    if (dev->port->send((const uint8_t *)buf, (uint16_t)cmd_len) != 0)
    {
        /* 发送失败按 ERROR 报(当前 send 是轮询实现, 基本不会走到) */
        ret = AT_RESP_ERROR;
    }
    else if (xSemaphoreTake(dev->at_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
    {
        ret = dev->resp_status;   /* 解析任务给出的结论: OK / ERROR */
    }
    else
    {
        ret = AT_RESP_TIMEOUT;    /* 到点没等到任何结束标志 */
    }

    xSemaphoreGive(dev->at_lock); /* 单一出口: 成功失败都还锁 */
    return ret;
}

/*=====================================================================
 * 初始化: 注入 port + 建同步对象 + 启动解析任务
 * 只创建资源, 不做任何 I/O、不认识任何命令 —— 探活("AT")、关回显("ATE0")
 * 等模块上电序列归 module 层的 xxx_init() 负责。
 * 必须在任务上下文调用(内部创建 FreeRTOS 对象)
 *=====================================================================*/
int at_init(const AT_PORT *port)
{
    if (port == NULL)
        return -1;
    g_at_dev.port = port;   /* 注入后 port 表只读, core 不再改 */

    g_at_dev.at_lock = xSemaphoreCreateMutex();
    g_at_dev.at_sem  = xSemaphoreCreateBinary();
    if (g_at_dev.at_lock == NULL || g_at_dev.at_sem == NULL)
        return -1;

    g_at_dev.last_cmd_len = 0;

    /* port 的硬件初始化是可选的: 实现了就调, 没实现(NULL)就跳过 */
    if (port->init != NULL && port->init() != 0)
        return -1;

    /* 常驻解析任务: 从此每个字节都由它消费 */
    if (xTaskCreate(at_parse_task, "at_parse", 512, &g_at_dev,
                    osPriorityNormal, NULL) != pdPASS)
        return -1;

    return 0;
}

/* 上层读完整应答内容用 */
PAT_Device at_get_device(void)
{
    return &g_at_dev;
}
