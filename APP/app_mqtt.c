/*
 * app_mqtt.c —— MQTT 应用层: transport 桥接 + MQTT 会话任务
 *
 * 本文件分两部分:
 *   A. transport 三函数: Paho MQTTPacket 库与"网络"之间的唯一边界
 *      (和 at_core 只认 dev->port-> 同一个思想: 库不碰网络, 网络不认识MQTT)
 *   B. app_mqtt_task: MQTT 会话生命周期
 *      CONNECT(报到) → SUBSCRIBE(订阅) → 循环(PUBLISH/收报文/保活)
 *
 * 四拍模式贯穿全程(和 AT 命令的"发命令等回复"同构):
 *   Serialize打包 → transport发出 → MQTTPacket_read等类型 → Deserialize解包
 */
#include "app_mqtt.h"
#include "esp8266.h"
#include "at_port_uart2.h"   /* at_port_stm32_uart2: 上电序列用 */
#include "MQTTPacket.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "bsp_led.h"
#include "bsp_buzzer.h"
#include <stdio.h>
#include <string.h>


/*
 * 设备命令表: 表驱动分发的核心。
 * 新增设备 = 这里加一行 + 在 BSP 写 on/off 两个函数, 主循环永不改动。
 * 注意: dev_cmd_t 定义在 app_mqtt.h, 与 transport 函数声明同处一个头。
 */
static const dev_cmd_t g_dev_cmd_table[] =
{
    { "led",    led_on,    led_off    },
    { "buzzer", buzzer_on, buzzer_off },
};

/*
 * 表驱动命令分发: payload 形如 "led on" / "buzzer off"。
 * 四步: 拷贝 → 切词(按空格) → 查表(strcmp) → 执行(handler)。
 *
 * 为什么先拷贝: payload 指向 buf 内部, 只有 len 个字节合法, 且【没有'\0'
 * 结尾】—— 直接 strchr/strcmp 会越界扫描; 且后续要写 '\0' 切词, 不能在
 * 只读的原始缓冲上动刀。所以先拷进自己的 temp 并手动补终止符。
 */
static void dev_cmd_exec(const char *payload, int len)
{
    int i;
    int n = sizeof(g_dev_cmd_table) / sizeof(g_dev_cmd_table[0]);  /* 条目数自动跟随表长 */
    char *index, *dev, *cmd;
    char temp[32];

    /* 1) 拷贝: 越界防护 + memcpy + 手动补 '\0'(三件套缺一不可) */
    if (len <= 0 || len >= (int)sizeof(temp))
        return;
    memcpy(temp, payload, len);
    temp[len] = '\0';

    /* 2) 切词: 第一个空格处截断 → dev=temp, cmd=空格后 */
    index = strchr(temp, ' ');
    if (index == NULL)          /* 没有空格: 如只发了 "on", 格式不对 */
    {
        printf("bad cmd, use: <dev> <on|off>\r\n");
        return;
    }
    *index = '\0';
    dev = temp;
    cmd = index + 1;

    /* 3)+4) 查表 + 执行: 全表走完都不匹配才报未知(提示在循环外) */
    for (i = 0; i < n; i++)
    {
        if (strcmp(dev, g_dev_cmd_table[i].dev) != 0)
            continue;           /* 不是这个设备, 看下一项 */

        /* 设备匹配: 处理动作 */
        if (strcmp(cmd, "on") == 0)
        {
            g_dev_cmd_table[i].on();
            return;
        }
        if (strcmp(cmd, "off") == 0)
        {
            g_dev_cmd_table[i].off();
            return;
        }
        printf("cmd not support: %s (dev=%s, use on/off)\r\n", cmd, dev);
        return;
    }

    /* 循环正常退出 i==n: 全表无此设备。注意退出值边界: 是 i>=n 而非 i>n */
    printf("dev not support: %s (use: led, buzzer)\r\n", dev);
}


/*=====================================================================
 * A. transport 桥接 —— Paho 通过这 3 个函数触达网络
 *=====================================================================*/

/* Paho 语义: "把 len 字节发出去", 成功返回发送的字节数。
 * 我们的 esp8266_send 是"成功返回0"语义, 这里做一次语义翻译。 */
int transport_sendPacketBuffer(int sock, unsigned char *buf, int len)
{
    (void)sock;              /* CIPMUX=0 单连接: 模块替我们记账, 句柄是摆设 */
    if (esp8266_send(buf, (uint16_t)len) == 0)
        return len;
    return -1;
}

/* Paho 语义: "给我 count 字节, 少一个都不行"。
 * 它先以 count=1 读包头, 再以 count=剩余长度读包体 —— 少给就错位。
 * 内部循环 esp8266_recv 直到读满; 超时/失败返回 -1, MQTTPacket_read 会
 * 以失败告终, 由上层按"本轮无报文"处理。 */
int transport_getdata(unsigned char *buf, int count)
{
    int got = 0;
    while (got < count)
    {
        int n = esp8266_recv(buf + got, (uint16_t)(count - got), 3000);
        if (n <= 0)
            return -1;       /* 3s 窗口内没凑齐: 当作本轮无报文 */
        got += n;
    }
    return got;
}

/* Paho 语义: 关闭底层连接。
 * TODO(以后): 发 AT+CIPCLOSE; 当前演示期 TCP 由 esp8266 层全生命周期持有 */
int transport_close(int sock)
{
    (void)sock;
    return 0;
}

/*=====================================================================
 * B. MQTT 会话任务
 *=====================================================================*/

/* broker 配置: 路线A=公共服务器(匿名,零配置)。
 * 换路线B(自建emqx): host 改成电脑局域网 IP, 注意 Windows 防火墙放行 1883 */
#define MQTT_BROKER_HOST  "broker.emqx.io"
#define MQTT_BROKER_PORT  1883
#define MQTT_CLIENT_ID    "f407-board-01"   /* ★全网唯一: 重复ID会被broker踢旧的 */
#define TOPIC_PUB         "f407/board/hello"    /* 板子→世界: 每5秒报个到 */
#define TOPIC_SUB         "f407/led/control"    /* 世界→板子: 收 "on"/"off" */

void app_mqtt_task(void *argument)
{
    unsigned char buf[256];
    int len;
    int pub_count = 0;

    /*---------------------------------------------------------------
     * 阶段1: 网络上电序列(module 层, 你已写好并验收)
     *---------------------------------------------------------------*/
    printf("esp8266 initing......\r\n");
    if (esp8266_init(&at_port_stm32_uart2) != 0) {
        printf("esp8266 init fail\r\n");
        vTaskDelete(NULL);
    }
    printf("esp8266_init OK\r\n");

    if (esp8266_connect_ap("your-ssid", "your-password") != 0) {  /* 本地运行: 换成你的WiFi信息(勿提交真值) */
        printf("connect ap fail\r\n");
        vTaskDelete(NULL);
    }
    esp8266_get_ip(at_get_device());

    /* TCP 层连 broker: 这是"走进营业厅大门", MQTT 报到还在后面 */
    if (esp8266_tcp_connect(MQTT_BROKER_HOST, MQTT_BROKER_PORT) != 0) {
        printf("tcp connect broker fail\r\n");
        vTaskDelete(NULL);
    }

    /*---------------------------------------------------------------
     * 阶段2: MQTT CONNECT —— 协议层报到(四拍之一)
     *   报身份(clientID)/约版本/约保活; 匿名模式下不携带账号密码。
     *   CONNACK rc=0 才算 broker 接受。
     *---------------------------------------------------------------*/
    {
        MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
        unsigned char sessionPresent, connack_rc;

        data.MQTTVersion       = 4;               /* 协议版本 3.1.1 */
        data.clientID.cstring  = MQTT_CLIENT_ID;
        data.keepAliveInterval = 60;              /* 60s 内要有报文或 PINGREQ */

        len = MQTTSerialize_connect(buf, sizeof(buf), &data);      /* ① 打包 */
        if (len <= 0) {
            printf("serialize connect fail\r\n");
            vTaskDelete(NULL);
        }
        if (transport_sendPacketBuffer(0, buf, len) != len) {      /* ② 发出 */
            printf("send connect fail\r\n");
            vTaskDelete(NULL);
        }
        if (MQTTPacket_read(buf, sizeof(buf), transport_getdata)    /* ③ 等类型 */
                != CONNACK) {
            printf("expect CONNACK fail\r\n");
            vTaskDelete(NULL);
        }
        if (MQTTDeserialize_connack(&sessionPresent, &connack_rc,   /* ④ 解包 */
                                    buf, sizeof(buf)) != 1 || connack_rc != 0) {
            printf("broker rejected, rc=%d\r\n", connack_rc);       /* 2=ID冲突 等 */
            vTaskDelete(NULL);
        }
        printf("MQTT connected\r\n");
    }

    /*---------------------------------------------------------------
     * 阶段3: SUBSCRIBE —— 订阅控制主题(四拍之二, 模式同上)
     *---------------------------------------------------------------*/
    {
        MQTTString topic = MQTTString_initializer;
        int msgid = 1, req_qos = 0, subscount = 0, granted_qos = -1;
        unsigned short packetid = (unsigned short)msgid;  /* suback解包: 参数类型是 unsigned short* */

        topic.cstring = TOPIC_SUB;
        len = MQTTSerialize_subscribe(buf, sizeof(buf), 0, msgid, 1,
                                      &topic, &req_qos);              /* ① 打包 */
        transport_sendPacketBuffer(0, buf, len);                      /* ② 发出 */
        if (MQTTPacket_read(buf, sizeof(buf), transport_getdata)      /* ③ 等类型 */
                != SUBACK) {
            printf("expect SUBACK fail\r\n");
            vTaskDelete(NULL);
        }
        MQTTDeserialize_suback(&packetid, 1, &subscount, &granted_qos, /* ④ 解包: 包id/数量/授予QoS */
                               buf, sizeof(buf));
        printf("suback: granted_qos=%d (%s)\r\n", granted_qos,
               granted_qos == 0 ? "OK" : "拒绝/0x80");
    }

    /*---------------------------------------------------------------
     * 阶段4: 主循环 —— PUBLISH 报到 + 收 PUBLISH 控 LED + 保活
     *---------------------------------------------------------------*/
    {
        MQTTString pub_topic = MQTTString_initializer;
        pub_topic.cstring = TOPIC_PUB;

        for (;;)
        {
            /* a) PUBLISH: QoS0 报个到。QoS0 无需 PUBACK, 发完即忘 */
            char payload[32];
            int payloadlen = snprintf(payload, sizeof(payload),
                                      "hello %d", pub_count++);
            len = MQTTSerialize_publish(buf, sizeof(buf), 0, 0, 0, 0,
                                        pub_topic, (unsigned char *)payload,
                                        payloadlen);
            transport_sendPacketBuffer(0, buf, len);
            printf("publish: %s\r\n", payload);

            /* b) 听一条报文: getdata 内部 3s 超时 → 本轮最多等 3 秒。
             *    返回值 = 报文类型枚举; <=0 = 窗口内无完整报文 */
            int type = MQTTPacket_read(buf, sizeof(buf), transport_getdata);

            if (type == PUBLISH)
            {
                unsigned char dup, retained;          /* 真实原型: 这两个是 unsigned char* */
                unsigned short packetid;              /* 真实原型: unsigned short* */
                int qos, payloadlen;
                MQTTString topicName;
                unsigned char *payload_in;
                MQTTDeserialize_publish(&dup, &qos, &retained, &packetid,
                                        &topicName, &payload_in, &payloadlen,
                                        buf, sizeof(buf));
                printf("rx: %.*s\r\n", payloadlen, (const char *)payload_in);

                /* 注意区分: payload=发送缓冲("hello N"), payload_in=收到的命令("led on") */
                dev_cmd_exec((const char *)payload_in, payloadlen);
            }
            /* type<=0: 本轮 3s 无报文 → 忽略。
             * 保活说明: 当前每5s的PUBLISH本身就是报文, broker收到任何报文都会
             * 重置keepalive计时器, 无需PINGREQ/PINGRESP。将来上报频率降到接近
             * keepalive时, 才需要"主动发PINGREQ→等PINGRESP"(记入扩展清单)。 */

            vTaskDelay(pdMS_TO_TICKS(2000));
            /* 节奏: publish+听 ≈ 每 5s 一轮 << keepalive 60s, 保活无忧 */
        }
    }
}
