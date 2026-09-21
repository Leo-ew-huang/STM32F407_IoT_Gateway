#include "esp8266.h"
#include <stdio.h>
#include <string.h>

int esp8266_init(const AT_PORT *port)
{
    int i;
    if (at_init(port) != 0 ) 
        return -1 ;
    /* 探活 */
    for(i = 0; i < 10; i++)
    {
        if(at_exec_cmd("AT", 500) == AT_RESP_OK)
            break;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if(i >= 10)
        return -1;

    /*  关闭回显 
        即使失败，core层也有兜底，只是respbuf中多了回显的内容
    */
    at_exec_cmd("ATE0", 500);

    return 0;
}

int esp8266_connect_ap(const char *ssid, const char *passwd)
{
    PAT_Device pDev = at_get_device();

    /* 设置为STA模式 */
    if(at_exec_cmd("AT+CWMODE=1", 2000) != AT_RESP_OK)
        return -1;

    /* 连接WIFI */
    char cmd[AT_CMD_BUF_LEN];
    int n = snprintf (cmd, sizeof (cmd), "AT+CWJAP=\"%s\",\"%s\"" , ssid, passwd); 
    if (n < 0 || n >= ( int ) sizeof (cmd))
        return -1 ;

    if(at_exec_cmd(cmd, 20000) == AT_RESP_OK)
    {
        printf("%s", (char *)pDev->resp_buf);
        return 0;
    }
    else
        return -1;
}

int esp8266_get_ip(PAT_Device pDev)
{
    uint8_t ip_buf[16];
    char *index;
    int i = 0;

    if(at_exec_cmd("AT+CIPSTA?", 500) == AT_RESP_OK)
    {
        index = strstr((char *)pDev->resp_buf, "+CIPSTA:ip:\"");
        if(index != NULL)
        {
            index += strlen("+CIPSTA:ip:\"");
            while(*index != '\"' && *index != '\0' && i < 15)
            {
                ip_buf[i++] = *index++;
            }
            ip_buf[i] = '\0';
            printf("connected ip is : %s\n", ip_buf);
            return 0;
        }
        else
        {
            printf("get ip info fail\n");
            return -1;
        }
    }
    else
    {
        printf("get ip info fail\n");
        return -1;
    }
}

int esp8266_tcp_connect(const char *host, uint16_t port)
{
    char cmd[AT_CMD_BUF_LEN];

    printf("set one connect mode......\n");
    if(at_exec_cmd("AT+CIPMUX=0", 500) != AT_RESP_OK)
        return -1;

    int n = snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%d", host, port);
    if (n < 0 || n >= ( int ) sizeof (cmd))
        return -1 ;

    printf("TCP connecting......\n");
    if(at_exec_cmd(cmd, 10000) != AT_RESP_OK)
    {
        printf("TCP connect fail\n");
        return -1;
    }
    printf("TCP connected\n");
    return 0;
}

int esp8266_send(const uint8_t *data, uint16_t len)
{
    char cmd[AT_CMD_BUF_LEN];

    int n = snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%u", len);
    if (n < 0 || n >= ( int ) sizeof (cmd))
        return -1 ;
    
    if(at_exec_cmd(cmd, 10000) != AT_RESP_OK)
        return -1;

    if(at_send_raw(data, len) != 0)
        return -1;

    if(xSemaphoreTake(at_get_device()->at_sem, pdMS_TO_TICKS(2000)) != pdTRUE)
        return -1;
    
    return 0;
}

int esp8266_recv(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
   return at_net_recv(buf, len, timeout_ms);
}
