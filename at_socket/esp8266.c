#include "esp8266.h"
#include <stdio.h>
#include <string.h>

static void esp8266_parse(void *arg);
static PAT_Socket get_socket_for_hw_socket(int hw_socket);
static int get_unused_hw_socket(void);
static void esp8266_parse_packet(PAT_Device ptDev);

static AT_Device g_esp8266_device =
{
	.name = "ESP8266",
};

int esp8266_connect_ap(char *ssid, char *passwd)
{
    PAT_Device ptDev = at_get_device();
    char cmd[128];
    
    // 1. 设置WiFi模式为STA
    if (at_exec_cmd(ptDev, "AT+CWMODE=1", NULL, 0, NULL, AT_TIMEOUT)) {
        return -1;
    }
    
    // 2. 连接AP
    if (passwd) {
        snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", ssid, passwd);
    } else {
        snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"\"", ssid);
    }
    
    if (at_exec_cmd(ptDev, cmd, NULL, 0, NULL, 10000)) { // 10秒超时
        return -1;
    }
    
    // 3. 查询IP地址(可选)
    if (at_exec_cmd(ptDev, "AT+CIFSR", NULL, 0, NULL, AT_TIMEOUT)) {
        return -1;
    }
    
    return 0;
}


int esp8266_init(char *uart_dev)
{
	//	绑定UART设备并初始化
	g_esp8266_device.ptUARTDev = GetUARTDevice(uart_dev);
 	if (g_esp8266_device.ptUARTDev == NULL)
        return -1;
	g_esp8266_device.ptUARTDev->Init(g_esp8266_device.ptUARTDev, 115200, 8, 'N', 1);

	//	初始化互斥量
	g_esp8266_device.at_lock = xSemaphoreCreateMutex();
	if (g_esp8266_device.at_lock == NULL)	
	{
		return -1;
	}

	//	初始化信号量
	g_esp8266_device.at_resp_sem = xSemaphoreCreateBinary();
	if (g_esp8266_device.at_resp_sem == NULL)	
	{
		return -1;
	}

	// 创建后台解析任务
	BaseType_t ret = xTaskCreate(esp8266_parse, "esp8266_parse", AT_PARSER_TASK_STACK, &g_esp8266_device, osPriorityNormal, NULL);
	if (ret != pdTRUE)
	{
		return -1;
	}
	
	// 等待模块就绪(发送AT直到返回OK, 兼容模块上电慢的情况)
	for (int i = 0; i < 10; i++)
	{
		if (at_exec_cmd(&g_esp8266_device, "AT", NULL, 0, NULL, 500) == 0)
			break;
		vTaskDelay(500);
	}

	// 复位ESP8266
   at_exec_cmd(&g_esp8266_device, "AT+RST", NULL, 0, NULL, AT_TIMEOUT);

   vTaskDelay(2000);

   // 关闭回显
   at_exec_cmd(&g_esp8266_device, "ATE0", NULL, 0, NULL, AT_TIMEOUT);

   // 禁用开机自动连接上次的WiFi(减小上电瞬间的电流尖峰, 部分固件不支持则忽略)
   at_exec_cmd(&g_esp8266_device, "AT+CWAUTOCONN=0", NULL, 0, NULL, AT_TIMEOUT);

	return 0;
}


int esp8266_socket(int domain, int type, int protocol)
{
	int i;
	for(i = 0; i < AT_DEVICE_SOCKETS_NUM; i++)
	{
		if(g_esp8266_device.sockets[i].used == 0)
		break;
	}
	
	if(i >= AT_DEVICE_SOCKETS_NUM)
	{
		return -1;
	}

	// 初始化 AT_Socket
	g_esp8266_device.sockets[i].used = 1;
	g_esp8266_device.sockets[i].busy = 0;
	g_esp8266_device.sockets[i].type = type;

	if(g_esp8266_device.sockets[i].at_packet_sem == NULL)	
	{
		g_esp8266_device.sockets[i].at_packet_sem = xSemaphoreCreateBinary();
		if (g_esp8266_device.sockets[i].at_packet_sem == NULL)	
		{
			return -1;
		}
	}

	if(g_esp8266_device.sockets[i].recv_queue == NULL)	
	{
		g_esp8266_device.sockets[i].recv_queue = xQueueCreate(AT_RECV_BUF_SIZE, sizeof(uint8_t));
		if (g_esp8266_device.sockets[i].recv_queue == NULL)	
		{
			return -1;
		}
	}
	else
	{
		// 槽位复用: 清空上次连接残留的数据。
		// 否则新连接会读到旧请求(如 "GET /api/state HTTP/1.1..."),
		// 导致方法/路径解析错乱(405 Method Not Allowed)或响应错乱。
		uint8_t ch;
		while (xQueueReceive(g_esp8266_device.sockets[i].recv_queue, &ch, 0) == pdTRUE)
			;
	}

	return i;
}

int esp8266_closesocket(int socket)
{
	PAT_Device ptDev = at_get_device();
    PAT_Socket ptSocket = &ptDev->sockets[socket];	

    int hw_socket = (int)(unsigned long)ptSocket->user_data;

    /* 仅当槽位仍被占用时才向模块发 CIPCLOSE。
     * 若槽位已被 CLOSED 通知释放(used=0, user_data 被清空), 连接已关闭,
     * 无需也不应再发 CIPCLOSE(尤其是绝不能对已失效槽位发 CIPCLOSE=0)。
     * 注意: 本模块固件客户端连接从 link 0 开始分配, hw=0 是真实客户端,
     * 应正常执行 CIPCLOSE=0; 否则该连接会一直残留在模块侧, 反复被 accept()
     * 选中且无数据, 把单线程服务器长时间拖住(表现为轮询/按钮长时间无响应)。 */
    if (ptSocket->used && hw_socket >= 0)
    {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "AT+CIPCLOSE=%d", hw_socket);

        /* 无论CIPCLOSE结果如何, 都要释放软件socket槽位, 避免重试时socket泄漏 */
        at_exec_cmd(ptDev, cmd, NULL, 0, NULL, AT_TIMEOUT);
    }
    
    ptSocket->busy = 0;
    ptSocket->used = 0;  
    ptSocket->user_data = NULL;

    return 0;
}

int esp8266_bind(int socket, const struct sockaddr *name, socklen_t namelen)
{
	PAT_Device ptDev = at_get_device();
    PAT_Socket ptSocket = &ptDev->sockets[socket];	
    
    ptSocket->local = *name;
    return 0;
}

int esp8266_listen(int socket, int backlog)
{
	PAT_Device ptDev = at_get_device();
	if (ptDev == NULL)
		return -1;

	AT_Socket *pSocket = &ptDev->sockets[socket];

	//	设置为多连接模式
	char *cmd = "AT+CIPMUX=1";
	if(at_exec_cmd(ptDev, cmd, NULL, 0, NULL, AT_TIMEOUT))
		return -1;

	//	设置服务器允许建立的最大连接数
	char *cmd2 = "AT+CIPSERVERMAXCONN=5";
	at_exec_cmd(ptDev, cmd2, NULL, 0, NULL, AT_TIMEOUT);

	//	建立服务器和监听端口
	char cmd3[128];
	struct sockaddr_in *pAddr = (struct sockaddr_in *)&pSocket->local;
	uint16_t port = ntohs(pAddr->sin_port);
	snprintf(cmd3, sizeof(cmd3), "AT+CIPSERVER=1,%d", port);
	if(at_exec_cmd(ptDev, cmd3, NULL, 0, NULL, AT_TIMEOUT))
		return -1;

	return 0;
}

int esp8266_accept(int socket, struct sockaddr *name, socklen_t *namelen)
{
	PAT_Device ptDev = at_get_device();
	if (ptDev == NULL)
		return -1;

	AT_Socket *pSocket = &ptDev->sockets[socket];
	struct sockaddr_in *pAddr = (struct sockaddr_in *)&pSocket->local;
	uint16_t server_port = ntohs(pAddr->sin_port);

	if(at_exec_cmd(ptDev, "AT+CIPSTATUS", NULL, 0, NULL, AT_TIMEOUT))
		return -1;

	// 解析返回数据
    // STATUS:<stat>
    // +CIPSTATUS:<link ID>,<type>,<remote IP>,<remote port>,<local port>,<tetype>
    // 注意: 服务器自身在 CIPMUX 模式下占 link 0, 其 remote port 为 0, 应跳过。
	{
        char *line = strstr((char *)ptDev->resp, "+CIPSTATUS:");
        while (line)
        {
            uint16_t hw_socket;
            char type[10];
            char remote_ip[32];
            uint16_t remote_port;
            uint16_t local_port;
            int tetype = 0;

            int parsed = sscanf(line, "+CIPSTATUS:%hu,%9[^,],%31[^,],%hu,%hu,%d",
                               &hw_socket, type, remote_ip, &remote_port, &local_port, &tetype);
            if (parsed >= 5)
            {
                // 跳过服务器自身监听连接(远端端口为 0)以及非目标端口的连接
                if (remote_port == 0 || local_port != server_port)
                {
                    line = strstr(line + 1, "+CIPSTATUS:");
                    continue;
                }

                PAT_Socket existed = get_socket_for_hw_socket(hw_socket);
                if (existed != NULL)
                {
                    // 该连接可能已由数据到达时自动分配(esp8266_parse_packet),
                    // 直接返回已有的软件 socket, 避免重复创建。
                    existed->busy = 1;   /* 服务器任务开始处理, 阻止 CLOSED 释放 */
                    if (name)
                    {
                        struct sockaddr_in *addr = (struct sockaddr_in *)name;
                        memset(addr, 0, sizeof(*addr));
                        addr->sin_family = AF_INET;
                        addr->sin_port = htons(remote_port);
                        inet_pton(AF_INET, remote_ip, &addr->sin_addr);
                    }
                    if (namelen)
                        *namelen = sizeof(struct sockaddr_in);

                    return (int)(existed - ptDev->sockets);
                }

                // 分配设置socket结构体
                int sw_socket = esp8266_socket(AF_INET, SOCK_STREAM, 0);
                if (sw_socket < 0)
                    return -1;

                PAT_Socket ptSocket = &ptDev->sockets[sw_socket];
                ptSocket->user_data = (void *)hw_socket;
                ptSocket->busy = 1;      /* 服务器任务开始处理, 阻止 CLOSED 释放 */

                if (name)
                {
                    struct sockaddr_in *addr = (struct sockaddr_in *)name;
                    memset(addr, 0, sizeof(*addr));
                    addr->sin_family = AF_INET;
                    addr->sin_port = htons(remote_port);
                    inet_pton(AF_INET, remote_ip, &addr->sin_addr);
                }
                if (namelen)
                    *namelen = sizeof(struct sockaddr_in);

                {
                    struct sockaddr_in *addr = (struct sockaddr_in *)&ptSocket->remote;
                    addr->sin_family = AF_INET;
                    addr->sin_port = htons(remote_port);
                    inet_pton(AF_INET, remote_ip, &addr->sin_addr);

                    addr = (struct sockaddr_in *)&ptSocket->local;
                    addr->sin_family = AF_INET;
                    addr->sin_port = htons(local_port);
                }

                return sw_socket;
            }
            line = strstr(line + 1, "+CIPSTATUS:");
        }
    }

	return -1;
}


int esp8266_connect(int socket, const struct sockaddr *name, socklen_t namelen)
{
	PAT_Device ptDev = at_get_device();
    PAT_Socket ptSocket = &ptDev->sockets[socket];

	// AT+CIPSTART=<link ID>,<type>,<remote IP>,<remote port>[,<TCP keep alive>]
    // 1. 找出一个空闲的link ID
    int link_id = get_unused_hw_socket();
    if (link_id < 0) {
        return -1;
    }

    // 2. 使用带link ID的CIPSTART必须处于多连接模式(CIPMUX=1)
    //    ESP8266默认CIPMUX=0(单连接), 此时带link ID的命令会报ERROR
    at_exec_cmd(ptDev, "AT+CIPMUX=1", NULL, 0, NULL, AT_TIMEOUT);

    // 3. 构造AT命令(注意: type和IP必须带引号, 部分固件不带引号会报ERROR)
    char cmd[64] = {0};
    char ipstr[16] = {0};
    struct sockaddr_in *paddr = (struct sockaddr_in *)name;
    uint16_t port = ntohs(paddr->sin_port);
    ipaddr_to_ipstr(name, ipstr);

    if (ptSocket->type == SOCK_STREAM)
        sprintf(cmd, "AT+CIPSTART=%d,\"TCP\",\"%s\",%d\r\n", link_id, ipstr, port);
    else
        sprintf(cmd, "AT+CIPSTART=%d,\"UDP\",\"%s\",%d\r\n", link_id, ipstr, port);

    // 4. 发送AT命令
    if (at_exec_cmd(ptDev, cmd, NULL, 0, NULL, AT_TIMEOUT)) {
        return -1;
    }

    // 记录硬件连接号
    ptSocket->user_data = (void *)link_id;

    return 0;
}

int esp8266_sendto(int socket, const void *data, size_t size, int flags, const struct sockaddr *to, socklen_t tolen)
{
	PAT_Device ptDev = at_get_device();
	PAT_Socket ptSocket = &ptDev->sockets[socket];

	/* 保护: 槽位可能已被迟到的 CLOSED 通知释放(used=0), 此时不能向该槽位发送。
	 * 注意: 本模块固件中客户端连接从 link 0 开始分配(串口可见 "0,CONNECT" 与
	 * "+CIPSTATUS:0,...,remote_port,..."), 因此 hw=0 是真实客户端连接, 必须允许发送。
	 * 早期代码用 "hw_socket<=0" 拦截, 导致所有落在 link 0 的请求响应发不出去
	 * (网页时好时坏 / 点击按钮无反应), 现改为以 used 标志判断槽位是否有效。 */
	if (ptSocket->used == 0)
		return -1;

	int hw_socket = (int)(unsigned long)ptSocket->user_data;

	// 对于TCP连接: AT+CIPSEND=<link ID>,<length>
    // 对于UDP:     AT+CIPSEND=[<link ID>,]<length>[,<remote IP>,<remote port>]	

	char cmd[64] = {0};
	if(ptDev->sockets[socket].type == SOCK_DGRAM && to != NULL)
	{
		char ipstr[16] = {0};
		struct sockaddr_in *paddr = (struct sockaddr_in *)to;
		uint16_t port = ntohs(paddr->sin_port);
		ipaddr_to_ipstr(to, ipstr);
		sprintf(cmd, "AT+CIPSEND=%d,%d,\"%s\",%d\r\n", hw_socket, size, ipstr, port);
	}
	else
	{
		sprintf(cmd, "AT+CIPSEND=%d,%d\r\n", hw_socket, size);
	}
	
	if(at_exec_cmd(ptDev, cmd, NULL, 0, NULL, AT_TIMEOUT))
		return -1;

	if(at_send_datas(ptDev, (uint8_t *)data, size, AT_TIMEOUT))
		return -1;

	// 等待 ESP8266 回 "SEND OK"(解析任务在收到 "OK\r\n" 时释放信号量):
	// 确保本块数据已被模块完整接收后再发下一块 CIPSEND,
	// 否则上一块的 SEND OK 会与下一块的 '>' 提示符信号量混淆,
	// 导致数据被 ESP8266 当作 AT 命令吞掉(表现为响应内容开头丢失/错乱)。
	xSemaphoreTake(ptDev->at_resp_sem, AT_TIMEOUT);

	return 0;	
}

int esp8266_recvfrom(int socket, void *mem, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen)
{
	PAT_Device ptDev = at_get_device();
	PAT_Socket ptSocket = &ptDev->sockets[socket];
	uint8_t *pdata = (uint8_t *)mem;
	uint8_t data;
	size_t recv_len = 0;

	// 如果是UDP，则需要先进行连接
	if(ptSocket->type == SOCK_DGRAM && ptSocket->user_data == NULL)
	{
		if (from == NULL || fromlen == NULL)
			return -1;

		// 连接到远程地址
		if (esp8266_connect(socket, from, *fromlen) != 0)
			return -1;
	}

	// 读取可能遗留的数据
	while(xQueueReceive(ptSocket->recv_queue, &data, 0) == pdTRUE)
	{
        pdata[recv_len] = data;
        recv_len++;
        if (recv_len >= len)
            return recv_len;
    }
	if (recv_len > 0)
        return recv_len;

	// 2. 无数据则等待信号量
    if (xSemaphoreTake(ptSocket->at_packet_sem, portMAX_DELAY) != pdTRUE) {
        return -1;
    }

    // 3. 再次尝试从接收队列读取数据
    while (xQueueReceive(ptSocket->recv_queue, &data, 0) == pdTRUE) {
        pdata[recv_len] = data;
        recv_len++;
        if (recv_len >= len)
            return recv_len;
    }
	if (recv_len > 0)
        return recv_len;

    return -1;  // 读取失败
}


static void esp8266_parse(void *arg)
{
	PAT_Device ptDev = (PAT_Device)arg;
	struct UART_Device *ptUARTDev = ptDev->ptUARTDev;
	uint8_t data;
	uint8_t line_buf[AT_RESP_BUF_SIZE];
	int len = 0;

	while(1)
	{
		// 读取UART数据
		if(ptUARTDev->Recv(ptUARTDev, &data, (int)portMAX_DELAY) != 0)
			continue;

		if (len < AT_RESP_BUF_SIZE - 1)
		{
			line_buf[len++] = data;
			line_buf[len] = '\0';
		}

		// 解析网络数据 "+IPD,<link id>,<len>:<data>"
		if(strstr((char *)line_buf, "+IPD,"))
		{
			esp8266_parse_packet(ptDev);
			len = 0;
			memset(line_buf, 0, sizeof(line_buf));  /* 清空, 防止残留字节被误判 */
			continue;
		}

		// 检测 CIPSEND 的 '>' 提示符(单独出现, 无换行):
		// ESP8266 收到 "AT+CIPSEND=<link>,<len>" 后回 '>' 等待数据,
		// 此时应立即响应, 否则 at_exec_cmd 会等到超时, 数据永远发不出去。
		if (data == '>')
		{
			ptDev->resp_status = AT_RESP_OK;
			xSemaphoreGive(ptDev->at_resp_sem);
			len = 0;
			memset(line_buf, 0, sizeof(line_buf));
			continue;
		}

		 // 3. 处理AT命令响应数据
        if (data == '\n') {  // 检测到换行符
            // 处理 ESP8266 的连接关闭通知 "<link>,CLOSED":
            // 浏览器断开连接时若不释放软件 socket, 5 个槽位很快耗尽,
            // 之后新连接无法 accept, 网页表现为刷新超时(ERR_CONNECTION_TIMED_OUT)。
            if (strstr((char *)line_buf, "CLOSED") != NULL) {
                int hw = 0;
                if (sscanf((char *)line_buf, "%d,CLOSED", &hw) == 1 && hw >= 0) {
                    PAT_Socket ps = get_socket_for_hw_socket(hw);
                    if (ps) {
                        /* 注意: 该 link id 可能已被新连接复用(新请求数据已入队),
                         * 这条 CLOSED 是旧连接的迟到通知。
                         * 此时若直接释放槽位, 新连接请求数据随之丢失,
                         * 服务器读不到完整请求而超时关闭, 表现为操作随机失败。
                         * 仅当队列为空(旧连接残留)且未被服务器处理(busy==0)时才释放;
                         * 队列非空或正在处理的, 保留给服务器任务处理, 处理完 closesocket 会释放。 */
                        if (uxQueueMessagesWaiting(ps->recv_queue) == 0 && ps->busy == 0)
                        {
                            ps->used = 0;
                            ps->user_data = NULL;
                        }
                    }
                }
            }
            if (ptDev->resp_line_counts < AT_RESP_LINES_MAX) {
                // 把当前行追加到resp数组(保存完整响应, 方便上层获取如CIFSR的IP信息)
                int n = len;
                if (ptDev->resp_len + n > AT_RESP_BUF_SIZE - 1)
                    n = AT_RESP_BUF_SIZE - 1 - ptDev->resp_len;
                if (n > 0)
                    memcpy(ptDev->resp + ptDev->resp_len, line_buf, n);
                ptDev->resp_len += n;
                ptDev->resp[ptDev->resp_len] = '\0';  // 保证字符串可打印
                ptDev->resp_line_counts++;
            }

            /* 检测结束标志: 必须在清空 line_buf 之前执行!
             * (否则 "OK\r\n" 等结束标志被 memset 清掉, 信号量永不释放, 所有 AT 命令超时) */
            if (strstr((char *)line_buf, "OK\r\n") || strstr((char *)line_buf, "ERROR\r\n") ||
                strstr((char *)line_buf, "FAIL\r\n") || strstr((char *)line_buf, "WIFI CONNECT FAIL\r\n")) {
                if (strstr((char *)line_buf, "OK\r\n")) {
                    ptDev->resp_status = AT_RESP_OK;
                } else {
                    ptDev->resp_status = AT_RESP_ERROR;
                }
                /* 释放信号量通知命令完成 */
                xSemaphoreGive(ptDev->at_resp_sem);
            }

            len = 0;
            memset(line_buf, 0, sizeof(line_buf));  /* 清空当前行, 防止残留字节被误判 */
        }
	}

}



PAT_Device at_get_device(void)
{
	return &g_esp8266_device;
}

static PAT_Socket get_socket_for_hw_socket(int hw_socket)
{
    PAT_Device ptDev = at_get_device();

    for (int i = 0; i < AT_DEVICE_SOCKETS_NUM; i++)
	{
        if (ptDev->sockets[i].used && (int)ptDev->sockets[i].user_data == hw_socket) {
            return &ptDev->sockets[i];
        }
    }
    
    return NULL;
}

static int get_unused_hw_socket(void)
{
	PAT_Device ptDev = at_get_device();

	uint8_t used_hw_sockets[AT_DEVICE_SOCKETS_NUM] = {0};

	for (int i = 0; i < AT_DEVICE_SOCKETS_NUM; i++)
	{
		if (ptDev->sockets[i].used)
		{
			int hw_socket = (int)ptDev->sockets[i].user_data;
			if (hw_socket >= 0 && hw_socket < AT_DEVICE_SOCKETS_NUM) {
				used_hw_sockets[hw_socket] = 1;
			}
		}
			
	}
	
	for (int i = 0; i < AT_DEVICE_SOCKETS_NUM; i++)
	{
		if (!used_hw_sockets[i])
		{
			return i;
		}
	}

	return -1;
}

static void esp8266_parse_packet(PAT_Device ptDev)
{
    struct UART_Device *ptUARTDev = ptDev->ptUARTDev;
    uint8_t data;
    int hw_socket = 0;
    int len = 0;
    int state = 0; // 0:等待hw_socket, 1:等待len, 2:读取数据
    int received = 0;
    int idle = 0;
    PAT_Socket ptSocket = NULL;

    // +IPD,<link id>,<len>:<data>
    while (1) {
        // 1. 读取串口数据(带超时保护: 若 +IPD 头部异常或数据不完整,
        //    不能永久阻塞在这里, 否则 AT 响应(如 '>'/SEND OK/CIPSTATUS)
        //    无人处理, Web 服务器会假死, 浏览器表现为连接超时)
        if (0 != ptUARTDev->Recv(ptUARTDev, &data, (int)RECV_PACKET_TIMEOUT)) {
            if (++idle > 3)   /* 连续 3 次超时(约3s)无数据, 放弃本次数据包 */
                return;
            continue;
        }
        idle = 0;

        switch (state) {
        case 0: // 解析hw_socket
            if (data == ',') {
                state = 1; // 切换到解析len状态

                // 根据hw_socket找到对应的PAT_Socket
                ptSocket = get_socket_for_hw_socket(hw_socket);
                if (!ptSocket) {
                    // 服务器模式下, 新连接的数据可能先于 accept() 到达。
                    // 这里自动创建软件 socket 绑定该硬件连接, 避免数据丢失。
                    int sw = esp8266_socket(AF_INET, SOCK_STREAM, 0);
                    if (sw >= 0) {
                        ptSocket = &ptDev->sockets[sw];
                        ptSocket->user_data = (void *)hw_socket;
                    }
                }

                continue;
            }
            if (data >= '0' && data <= '9') {
                hw_socket = hw_socket * 10 + (data - '0');
            }
            break;
            
        case 1: // 解析len
            if (data == ':') {
                state = 2; // 切换到读取数据状态                
                continue;
            }
            if (data >= '0' && data <= '9') {
                len = len * 10 + (data - '0');
                if (len > 2048)   /* 异常长度(ESP8266 单包不可能这么大), 放弃 */
                    return;
            }
            break;
            
        case 2: // 读取数据
            if (received < len) {
                if (ptSocket)
                    xQueueSend(ptSocket->recv_queue, &data, 0);
            }
            received++;
            if (received >= len) {                
                // 释放信号量通知有新数据
                if (ptSocket)
                    xSemaphoreGive(ptSocket->at_packet_sem);
                return;
            }
            break;
        }
    }
}
