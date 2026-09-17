#include "at_command.h"
#include <string.h>

int at_exec_cmd(PAT_Device ptDev, char *cmd, uint8_t *resp, uint32_t max_len, uint32_t *resp_len, uint32_t timeout)
{
    int ret = -1;
    struct UART_Device *ptUARTDev = ptDev->ptUARTDev;
    char cmd_line[260];
    int cmd_len;

    // 清空回应缓冲区
    ptDev->resp_len = 0;
    ptDev->resp_line_counts = 0;
    ptDev->resp_status = 0; // 默认回应状态为错误

    // 获得互斥锁
    if (xSemaphoreTake(ptDev->at_lock, portMAX_DELAY) != pdTRUE)
        return ret; // 获取锁失败

    // 清除上次可能残留的信号量, 避免旧回应污染本次结果
    xSemaphoreTake(ptDev->at_resp_sem, 0);

    // 补全AT命令结尾(ESP8266要求以\r\n结尾)
    cmd_len = (int)strlen(cmd);
    while (cmd_len > 0 && (cmd[cmd_len - 1] == '\r' || cmd[cmd_len - 1] == '\n'))
        cmd_len--;
    if (cmd_len > (int)sizeof(cmd_line) - 2)
        cmd_len = (int)sizeof(cmd_line) - 2;
    memcpy(cmd_line, cmd, cmd_len);
    cmd_line[cmd_len++] = '\r';
    cmd_line[cmd_len++] = '\n';

    // 发送AT命令
    ptUARTDev->Send(ptUARTDev, (uint8_t *)cmd_line, cmd_len, timeout);

    // 等待信号量(后台解析任务会在收到回应后释放这个信号量)
    if (xSemaphoreTake(ptDev->at_resp_sem, timeout) != pdTRUE)
    {
        ret = -1; // 超时
    }
    else
    {
        if(resp)
            memcpy(resp, ptDev->resp, ptDev->resp_len < max_len ? ptDev->resp_len : max_len);
        ret = ptDev->resp_status; // 0=OK, 1=ERROR
    }

    // 释放互斥锁
    xSemaphoreGive(ptDev->at_lock);

    return ret;
}

int at_send_datas(PAT_Device ptDev, uint8_t *datas, uint32_t data_len, uint32_t timeout)
{
    struct UART_Device *ptUARTDev = ptDev->ptUARTDev;
    int ret = -1;

    // 获得互斥锁
    if (xSemaphoreTake(ptDev->at_lock, portMAX_DELAY) != pdTRUE)
        return -1; // 获取锁失败

    // 发送数据
    ret = ptUARTDev->Send(ptUARTDev, datas, data_len, timeout);

    // 释放互斥锁
    xSemaphoreGive(ptDev->at_lock);

    return ret;
}
