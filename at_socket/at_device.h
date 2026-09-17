// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (c) 2008-2023 100askTeam : Dongshan WEI <weidongshan@qq.com> 
 * Discourse:  https://forums.100ask.net
 */
 
/*  Copyright (C) 2008-2023 深圳百问网科技有限公司
 *  All rights reserved
 *
 * 免责声明: 百问网编写的文档, 仅供学员学习使用, 可以转发或引用(请保留作者信息),禁止用于商业用途！
 * 免责声明: 百问网编写的程序, 可以用于商业用途, 但百问网不承担任何后果！
 * 
 * 本程序遵循GPL V3协议, 请遵循协议
 * 百问网学习平台   : https://www.100ask.net
 * 百问网交流社区   : https://forums.100ask.net
 * 百问网官方B站    : https://space.bilibili.com/275908810
 * 本程序所用开发板 : DShanMCU-H5
 * 百问网官方淘宝   : https://100ask.taobao.com
 * 联系我们(E-mail): weidongshan@qq.com
 *
 *          版权所有，盗版必究。
 *  
 * 修改历史     版本号           作者        修改内容
 *-----------------------------------------------------
 * 2024.09.01      v01         百问科技      创建文件
 *-----------------------------------------------------
 */

#ifndef __AT_DEVICE_H__
#define __AT_DEVICE_H__

#include <uart_device.h>
#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "task.h"
#include "semphr.h"

#define SHUT_RD     0
#define SHUT_WR     1
#define SHUT_RDWR   2 

#define IPPROTO_TCP 6	  /* Transmission Control Protocol	  */

/* Supported address families. */
#define AF_INET		2	/* Internet IP Protocol 	*/
#define PF_INET		AF_INET

/* Address to accept any incoming messages. */
#define	INADDR_ANY		((unsigned int) 0x00000000)

/* socket type */
#define SOCK_STREAM     1
#define SOCK_DGRAM	    2
#define SOCK_RAW	    3
#define SOCK_RDM	    4
#define SOCK_SEQPACKET	5
#define SOCK_DCCP	    6
#define SOCK_PACKET     10

/* 超时时间 */
#define AT_TIMEOUT 1000
/* 解析 +IPD 数据包时单字节读取超时: 必须远小于 AT_TIMEOUT。
 * 原 1000ms 会让解析任务在"残缺数据包"上阻塞最多 3s(1000ms*3次),
 * 期间 '>' / SEND OK / CIPSTATUS 等 AT 响应无人处理,
 * 导致 CIPSEND 发送超时、响应丢失, 表现为操作随机失败。 */
#define RECV_PACKET_TIMEOUT 200

#define AT_DEVICE_SOCKETS_NUM 5  /* W800支持多少个socket没有定论, esp8266只支持5个 */
#define AT_CLIENT_SOCKETS_NUM 5
#define AT_RECV_BUF_SIZE   1024  /* 接收队列容量: 浏览器请求头+body约300~500B, 留足余量避免截断丢请求 */
#define AT_RESP_BUF_SIZE   512   /* AT响应缓冲: CIPSTATUS 多连接时(STATUS+5行+OK)约300B, 原256B会被截断导致accept失败 */

#define AT_RESP_LINES_MAX 100

#define AT_PARSER_TASK_STACK 512

#define AT_RESP_OK    0				  /* AT response end is OK */
#define AT_RESP_ERROR 1 			  /* AT response end is ERROR */

#define PP_HTONS(x) ((((x) & 0x00ffUL) << 8) | (((x) & 0xff00UL) >> 8))
#define PP_NTOHS(x) PP_HTONS(x)
#define PP_HTONL(x) ((((x) & 0x000000ffUL) << 24) | \
                     (((x) & 0x0000ff00UL) <<  8) | \
                     (((x) & 0x00ff0000UL) >>  8) | \
                     (((x) & 0xff000000UL) >> 24))
#define PP_NTOHL(x) PP_HTONL(x)

#define htons(x) (uint16_t)PP_HTONS(x)
#define ntohs(x) (uint16_t)PP_NTOHS(x)
#define htonl(x) (uint32_t)PP_HTONL(x)
#define ntohl(x) (uint32_t)PP_NTOHL(x)

typedef uint32_t socklen_t;

struct sockaddr {
  uint8_t sa_len;
  uint8_t sa_family;
  char sa_data[14];
};

struct in_addr {
  uint32_t s_addr;
};

struct sockaddr_in {
  uint8_t sin_len;
  uint8_t sin_family;
  uint16_t sin_port;
  struct in_addr sin_addr;
#define SIN_ZERO_LEN 8
  char sin_zero[SIN_ZERO_LEN];
};


typedef struct AT_Socket {
	uint32_t used; /* 0-未被占用, 1-被占用 */
	uint32_t busy; /* 1-服务器任务正在处理该连接(读取/回复/关闭), 此时 CLOSED 通知不得释放 */
	int type;      /* TCP(SOCK_STREAM) or UDP(SOCK_DGRAM) */
	struct sockaddr local;  /* 用来记录本地IP/PORT */
	struct sockaddr remote; /* 用来记录远端IP/PORT */
	void *user_data;        /* AT模块自己的数据,
	                         * 对于W800就是硬件socket值,
	                         * 对于ESP8266就是link id 
	                         */
	SemaphoreHandle_t at_packet_sem; /* 读取网络数据时,等待这个信号量 */
	QueueHandle_t recv_queue;        /* 队列,用来存放接收到的网络数据 */
}AT_Socket, *PAT_Socket;

typedef struct AT_Device {
	char *name; /* WIFI模块的名字 */
	SemaphoreHandle_t at_lock;      /* 发送AT命令前需要先获得这个锁 */
	SemaphoreHandle_t at_resp_sem;  /* 发送AT命令后等待这个信号量(等待AT命令的回应) */
	uint8_t resp[AT_RESP_BUF_SIZE]; /* 存放AT命令的回应数据 */ 
	uint32_t resp_len;              /* AT命令回应数据的长度 */
	uint32_t resp_line_counts;      /* AT命令回应的数据有多少行 */
	uint32_t resp_status;           /* AT命令的回应是OK还是ERR */
	struct UART_Device *ptUARTDev;        /* 使用这个串口设备访问WIFI模块 */
	AT_Socket sockets[AT_DEVICE_SOCKETS_NUM]; /* socket结构体数组 */
}AT_Device, *PAT_Device;

#endif /* __AT_DEVICE_H__ */

