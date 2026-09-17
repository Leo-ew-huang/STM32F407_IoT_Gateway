
#ifndef _UART_DEVICE_H
#define _UART_DEVICE_H

#include <stdint.h>

struct UART_Device {
    char *name;
    int (*Init)(struct UART_Device *pDev, int baud, int datas, char parity, int stop);
    int (*Send)(struct UART_Device *pDev, uint8_t *datas, int len, int timeout_ms);
    int (*Recv)(struct UART_Device *pDev, uint8_t *data, int timeout_ms);
    void *priv_data;
};

struct UART_Device *GetUARTDevice(char *name);

/* UART2(ESP8266)通信统计, 用于调试"AT no response" */
extern volatile uint32_t g_uart2_rx_bytes;
extern volatile uint32_t g_uart2_tx_ok;
extern volatile uint32_t g_uart2_tx_fail;
extern volatile uint32_t g_uart2_err_cnt;


#endif


