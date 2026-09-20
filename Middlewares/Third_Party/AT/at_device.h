#ifndef AT_DEVICE_H
#define AT_DEVICE_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "semphr.h"

#define AT_RESP_BUF_LEN 256     /* 应答缓冲: 存一条命令的完整应答(多行拼接) */
#define AT_CMD_BUF_LEN  160     /* 命令文本最长长度(CWJAP 带账号密码够用) */

/*
 * 一条 AT 命令的执行结果。
 * 约定: 0 成功, 负数失败(与大多数 C 库风格一致)。
 */
typedef enum {
    AT_RESP_OK      =  0,       /* 应答以 OK 结束 */
    AT_RESP_TIMEOUT = -1,       /* 超时未等到结束标志 */
    AT_RESP_ERROR   = -2,       /* 应答以 ERROR/FAIL 结束 */
} AT_RespStatus;

/*
 * port 层接口表: AT core 与具体硬件之间唯一的边界。
 * core 层只通过这张表收发字节, 不知道底层是 STM32+UART2 还是别的芯片。
 * 换芯片/换串口 = 新写一个 port 实现, core 层一行不改。
 */
typedef struct AT_PORT
{
    /* 硬件收发初始化, 可选: 不需要就填 NULL, core 会跳过。成功返回 0 */
    int (*init)(void);
    /* 阻塞发送 len 字节。成功返回 0, 失败返回 -1 */
    int (*send)(const uint8_t *data, uint16_t len);
    /* 阻塞收 1 字节, 最多等 timeout_ms。成功返回 0, 超时返回 -1 */
    int (*recv_byte)(uint8_t *byte, uint32_t timeout_ms);
} AT_PORT, *PAT_PORT;


typedef struct AT_Device
{
    /* 硬件适配表: at_init 时注入一次, 之后只读(const 保护) */
    const AT_PORT *port;
    /* 互斥锁: 同一时刻只允许一条 AT 对话(at_exec_cmd 拿/还) */
    SemaphoreHandle_t at_lock;
    /* 二值信号量: 解析任务见 OK/ERROR 后 give, at_exec_cmd 阻塞等它 */
    SemaphoreHandle_t at_sem;
    /* 完整应答内容(多行拼接), 上层通过 at_get_device() 读取 */
    uint8_t resp_buf[AT_RESP_BUF_LEN];
    uint32_t resp_len;              /* resp_buf 里的有效字节数 */
    AT_RespStatus resp_status;      /* 最近一条命令的结果 */
    /* 刚发出的命令原文(无\r\n): 解析任务靠它识别并跳过回显行。
     * 回显免疫让 core 在不认识任何命令的前提下, 也不怕回显里的
     * OK/ERROR 子串误触发 —— 探活/ATE0 因此可以放心放去 module 层 */
    uint8_t  last_cmd[AT_CMD_BUF_LEN];
    uint16_t last_cmd_len;
} AT_Device, *PAT_Device;


/*
 * 初始化 AT 层: 注入 port 表 + 建同步对象 + 启动后台解析任务。
 * 只创建资源, 不做任何 I/O; 探活/关回显等命令序列归 module 层。
 * 必须在任务上下文调用。成功返回 0, 失败返回 -1。
 */
int at_init(const AT_PORT *port);

/*
 * 发送一条 AT 命令并阻塞等结果。
 * cmd 不要带 "\r\n"(内部补齐); timeout_ms 为等待应答的最长时间。
 * 返回 AT_RespStatus: AT_RESP_OK / AT_RESP_TIMEOUT / AT_RESP_ERROR。
 */
int at_exec_cmd(const char *cmd, uint32_t timeout_ms);

/* 获取设备句柄(读 resp_buf / resp_status 用) */
PAT_Device at_get_device(void);



#endif /*AT_DEVICE_H */ 
