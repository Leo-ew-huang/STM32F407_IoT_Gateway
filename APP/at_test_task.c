#include <stdio.h>
#include "at_test_task.h"
#include "at_device.h"
#include "at_port_uart2.h"

void at_test_task(void *argument)
{
    int r;
    if (at_init(&at_port_stm32_uart2) != 0) {
        printf("at_init fail\r\n");
        vTaskDelete(NULL);
    }
    printf("at_init OK\r\n");
    for (;;)
    {
        r = at_exec_cmd("AT", 1000);
        printf("AT -> %d, resp=[%s]\r\n", r, at_get_device()->resp_buf);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
