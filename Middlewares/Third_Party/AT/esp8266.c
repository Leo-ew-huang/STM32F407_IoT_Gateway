#include "esp8266.h"

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

    /* 设置为STA模式 */
    if(at_exec_cmd("AT+CWMODE=1", 500) != AT_RESP_OK)
        return -1;
    
    return 0;
}
