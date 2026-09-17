#ifndef __AT_COMMAND_H__
#define __AT_COMMAND_H__

#include "at_device.h"
#include "at_socket.h"
#include "FreeRTOS.h"

int at_exec_cmd(PAT_Device ptDev, char *cmd, uint8_t *resp, uint32_t max_len, uint32_t *resp_len, uint32_t timeout);

int at_send_datas(PAT_Device ptDev, uint8_t *datas, uint32_t data_len, uint32_t timeout);


#endif /* __AT_COMMAND_H__ */
