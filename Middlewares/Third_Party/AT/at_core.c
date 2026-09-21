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
#include "uart_ringbuf.h"   /* 复用环形缓冲做网络接收环(纯数据结构, 非硬件依赖) */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "cmsis_os.h"
#include <string.h>

/* 单例: 本工程只有一个 WIFI 模块, 不做设备注册表 */
static AT_Device g_at_dev;

/* 网络接收侧(+IPD 分流的 payload 落地处)。
 * core 私有: module 层只经 at_net_recv 取数, 不直接摸这两个对象 */
static struct RingBuffer          g_net_rb;      /* 网络接收环(256B, MQTT 初期够用) */
static SemaphoreHandle_t   g_net_sem;     /* "IPD 突发到了"的门铃 */
static volatile uint32_t   g_net_drop_cnt;/* 接收环满被丢弃的字节数(调试用) */

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
    /* +IPD,<len>:<data> 分流状态机: 0=正常行模式 1=解析长度 2=消费数据 */
    int ipd_state = 0;
    int ipd_len = 0;
    int ipd_cnt = 0;

    for (;;)
    {
        /* 阻塞收 1 字节; 200ms 没数据就转回去继续等, 攒了一半的行保留 */
        if (dev->port->recv_byte(&byte, 200) != 0)
            continue;

        /* ---- +IPD 分流状态机: 必须在所有行逻辑之前 ----
         * IPD 数据里可能有 \r\n 和 "OK" 等任意字节, 绝不能进攒行/应答判定 */
        if (ipd_state == 1)          /* 攒 "+IPD,<len>:" 的长度段 */
        {
            if (byte == ':')
            {
                ipd_state = 2;
                ipd_cnt = 0;
            }
            else if (byte >= '0' && byte <= '9')
            {
                ipd_len = ipd_len * 10 + (byte - '0');
                if (ipd_len > 2048)  /* 异常长度: 放弃本包, 回行模式自愈 */
                    ipd_state = 0;
            }
            continue;            /* 其他字符忽略 */
        }
        if (ipd_state == 2)      /* 消费 len 个网络数据字节 */
        {
            if (rb_write(&g_net_rb, byte) != 0)
                g_net_drop_cnt++;/* 环满: 丢弃并计数, 保住解析主流程不死 */
            if (++ipd_cnt >= ipd_len)
            {
                ipd_state = 0;
                xSemaphoreGive(g_net_sem);   /* "IPD 突发到了" */
            }
            continue;
        }

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
        {
            line[len++] = byte;

            /* 行首出现 "+IPD," → 后面是 <len>:<data>, 切入分流状态机。
             * len 归零: "+IPD," 前缀已被状态机接管, 不再属于任何应答行 */
            if (len == 5 && memcmp(line, "+IPD,", 5) == 0)
            {
                ipd_state = 1;
                ipd_len = 0;
                len = 0;
            }
        }
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

    /* 网络接收侧: 环形缓冲 + "有IPD突发"门铃 */
    g_net_sem = xSemaphoreCreateBinary();
    rb_init(&g_net_rb);
    g_net_drop_cnt = 0;
    if (g_net_sem == NULL)
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

/*=====================================================================
 * 裸发网络数据: CIPSEND 的 '>' 之后, 数据本体走这里。
 * 只拿锁防串发, 不等应答 —— SEND OK 行由解析任务吞掉,
 * 残留令牌由下次 exec_cmd 的清残留兜底(见 esp8266_send 的使用注释)。
 *=====================================================================*/
int at_send_raw(const uint8_t *data, uint16_t len)
{
    int ret;

    xSemaphoreTake(g_at_dev.at_lock, portMAX_DELAY);
    ret = g_at_dev.port->send(data, len);
    xSemaphoreGive(g_at_dev.at_lock);
    return ret;
}

/*=====================================================================
 * 从网络接收环取数据: 读满 len 字节或超时, 返回实际字节数。
 * 套路 = 先摸缓冲(唯一事实源), 摸空再按"剩余时间"等门铃,
 * 醒来/超时后回到循环顶再摸 —— 与 uart2_receive_blocking 同款。
 *=====================================================================*/
int at_net_recv(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    uint16_t   got   = 0;

    while (got < len)
    {
        if (rb_read(&g_net_rb, &buf[got]) == 0)
        {
            got++;
            continue;   /* 摸到了, 继续摸剩下的 */
        }

        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= pdMS_TO_TICKS(timeout_ms))
            break;      /* 总超时: 返回已读到的部分(可能为0) */

        xSemaphoreTake(g_net_sem, pdMS_TO_TICKS(timeout_ms) - elapsed);
    }
    return (int)got;
}

/* 上层读完整应答内容用 */
PAT_Device at_get_device(void)
{
    return &g_at_dev;
}
