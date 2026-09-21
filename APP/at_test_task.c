#include <stdio.h>
#include "at_test_task.h"
#include "at_device.h"
#include "at_port_uart2.h"
#include "esp8266.h"

void at_test_task(void *argument)
{
    int r;
    if (esp8266_init(&at_port_stm32_uart2) != 0) {
        printf("esp8266_init fail\r\n");
        vTaskDelete(NULL);
    }
    printf("esp8266_init OK\r\n");

    /* 连路由(失败打印原因并停住) */
    if (esp8266_connect_ap( "your-ssid" , "your-password" ) != 0 )
    {
        printf ( "connect_ap fail\r\n" );
        vTaskDelete( NULL );
    }
    esp8266_get_ip(at_get_device());
	/* WiFi 验证完成后: TCP 对话测试 */
    esp8266_tcp_connect("192.168.2.2", 8080);      /* xxx = 你电脑的 IP */
    esp8266_send((const uint8_t *)"hello net", 9);

    uint8_t rbuf[32];
    int n = esp8266_recv(rbuf, 9, 3000);
    printf("recv %d bytes: %.*s\r\n", n, n, (const char *)rbuf);

    for (;;)
    {
        r = at_exec_cmd("AT", 1000);
        printf("AT -> %d, resp=[%s]\r\n", r, at_get_device()->resp_buf);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
