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

#ifndef __AT_SOCKET_H__
#define __AT_SOCKET_H__

#include "at_device.h"

int at_socket(int domain, int type, int protocol);
int at_closesocket(int socket);
int at_shutdown(int socket, int how);
int at_bind(int socket, const struct sockaddr *name, socklen_t namelen);
int at_connect(int socket, const struct sockaddr *name, socklen_t namelen);
int at_sendto(int socket, const void *data, size_t size, int flags, const struct sockaddr *to, socklen_t tolen);
int at_send(int socket, const void *data, size_t size, int flags);
int at_recvfrom(int socket, void *mem, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen);
int at_recv(int socket, void *mem, size_t len, int flags);
int at_flush(int socket);
int at_listen(int socket, int backlog);
int at_accept(int socket, struct sockaddr *name, socklen_t *namelen);


#define socket(domain, type, protocol)                      at_socket(domain, type, protocol)
#define closesocket(socket)                                 at_closesocket(socket)
#define shutdown(socket, how)                               at_shutdown(socket, how)
#define bind(socket, name, namelen)                         at_bind(socket, name, namelen)
#define connect(socket, name, namelen)                      at_connect(socket, name, namelen)
#define sendto(socket, data, size, flags, to, tolen)        at_sendto(socket, data, size, flags, to, tolen)
#define send(socket, data, size, flags)                     at_send(socket, data, size, flags)
#define recvfrom(socket, mem, len, flags, from, fromlen)    at_recvfrom(socket, mem, len, flags, from, fromlen)
#define recv(socket, mem, len, flags)                       at_recv(socket, mem, len, flags)
#define net_flush(socket)                                   at_flush(socket)
#define listen(socket, backlog)                             at_listen(socket, backlog)
#define accept(socket, name, namelen)                       at_accept(socket, name, namelen)

/**********************************************************************
 * 函数名称： ipaddr_aton
 * 功能描述： 把字符串形式的IP地址转换为"struct in_addr"结构体
 * 输入参数： cp - 字符串形式的IP地址
 * 输出参数： addr - "struct in_addr"结构体指针
 * 返 回 值： 1-成功
 * 修改日期：	版本号	  修改人 	  修改内容
 * -----------------------------------------------
 * 2024/09/01		 V1.0	  韦东山 	  创建
 ***********************************************************************/
int ipaddr_aton(const char *cp, struct in_addr *addr);

/**********************************************************************
 * 函数名称： inet_pton
 * 功能描述： 将点分十进制的ip地址转化为用于网络传输的数值格式
 * 输入参数： family - 没有使用
 *            cp - 字符串形式的IP地址
 * 输出参数： addr - "struct in_addr"结构体指针
 * 返 回 值： 1-成功
 * 修改日期：	版本号	  修改人 	  修改内容
 * -----------------------------------------------
 * 2024/09/01		 V1.0	  韦东山 	  创建
 ***********************************************************************/
int inet_pton(int family, const char *cp, void *addr); 

/**********************************************************************
 * 函数名称： ipaddr_to_ipstr
 * 功能描述： 把"struct sockaddr"结构体里面的IP转换为字符串形式的IP
 * 输入参数： sockaddr - "struct sockaddr"结构体指针
 * 输出参数： ipstr - 字符串形式的IP地址
 * 返 回 值： 0-成功
 * 修改日期：	版本号	  修改人 	  修改内容
 * -----------------------------------------------
 * 2024/09/01		 V1.0	  韦东山 	  创建
 ***********************************************************************/
int ipaddr_to_ipstr(const struct sockaddr *sockaddr, char *ipstr);

/**********************************************************************
 * 函数名称： inet_ntop
 * 功能描述： 把"struct sockaddr"结构体里面的IP转换为字符串形式的IP
 * 输入参数： src - "struct sockaddr"结构体指针
 * 输出参数： dst - 字符串形式的IP地址
 * 返 回 值： NULL-失败, 成功则反复dst指针
 * 修改日期：	版本号	  修改人 	  修改内容
 * -----------------------------------------------
 * 2024/09/06		 V1.0	  韦东山 	  创建
 ***********************************************************************/
const char *inet_ntop(int af, const void *src, char *dst, socklen_t cnt);

/**********************************************************************
 * 函数名称： at_init
 * 功能描述： 初始化W800相关结构体
 * 输入参数： uart_dev - UART名称
 * 输出参数： 无
 * 返 回 值： 0-成功, (-1)-失败
 * 修改日期：	版本号	  修改人 	  修改内容
 * -----------------------------------------------
 * 2024/09/01		 V1.0	  韦东山 	  创建
 ***********************************************************************/
int at_init(char *uart_dev);

/**********************************************************************
 * 函数名称： at_connect_ap
 * 功能描述： 连接WIFI AP
 * 输入参数： ssid   - AP名称
 *            passwd - 密码
 * 输出参数： 无
 * 返 回 值： 0-成功, (-1)-失败
 * 修改日期：	版本号	  修改人 	  修改内容
 * -----------------------------------------------
 * 2024/09/04		 V1.0	  韦东山 	  创建
 ***********************************************************************/
int at_connect_ap(char *ssid, char *passwd);

#endif /* __AT_SOCKET_H__ */

