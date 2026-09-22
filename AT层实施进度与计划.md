# AT 层实施进度与计划（跨环境 Handoff 文档）

> 本文档供两台工作环境（公司/家）之间同步进度使用，也给下一个接手的 AI agent。
> 最后更新：2026-09-22（家里机·晚间）—— AT-16 下行控灯闭环达成；遗留作业=命令表驱动重构；公司环境明天接线

## 一、项目目标

STM32F407 + ESP8266(AT 固件)，最终实现 **MQTT 上云双向通信**：

- 路线：**AT+CIPSTART 建 TCP + 移植 Paho MQTT(embedded-c)**，不用 AT+MQTT*（固件大概率不支持），不自写 MQTT 协议
- 参考框架：`at_socket/`（百问网 GPL 库，**只参考不进编译**），其分层：at_socket.c(BSD薄壳) → esp8266.c(module) → at_command.c(core) → uart_device.c(port)
- 换模块/换芯片只改 port 层一个文件

## 二、三层架构（已定，不再动摇）

```
module 层  esp8266.c(以后写)      业务: 连WiFi/连TCP/发MQTT, AT命令序列
core 层   at_core.c(重写中)      攒行·判OK/ERROR·一问一答同步(锁+信号量)
port 层   at_port_uart2.c(用户写) init/send/recv_byte 3函数, 全库唯一认识UART2的文件
物理层    BSP/uart2_driver.c(已完成) DMA→暂存数组→RingBuffer→阻塞读字节, 硬件验收过
```

core 与 port 之间唯一的接口 = `at_device.h` 里的 `AT_PORT` 函数指针表（3 个函数）。

### 已定稿的接口（Middlewares/Third_Party/AT/at_device.h）

```c
typedef struct AT_PORT {
    int (*init)(void);                                            /* 可选, NULL跳过 */
    int (*send)(const uint8_t *data, uint16_t len);               /* 0成功/-1失败 */
    int (*recv_byte)(uint8_t *byte, uint32_t timeout_ms);         /* 0成功/-1超时 */
} AT_PORT, *PAT_PORT;

typedef struct AT_Device {
    const AT_PORT *port;          /* 注入的适配表, 只读 */
    SemaphoreHandle_t at_lock;    /* 互斥锁: 同一时刻只有一条AT对话 */
    SemaphoreHandle_t at_sem;     /* 二值信号量: 解析任务见OK/ERROR后give */
    uint8_t resp_buf[AT_RESP_BUF_LEN];  /* 256, 完整应答拼接 */
    uint32_t resp_len;
    AT_RespStatus resp_status;    /* OK=0 / TIMEOUT=-1 / ERROR=-2 */
} AT_Device, *PAT_Device;

int at_init(const AT_PORT *port);                    /* 注入port+建对象+探活, 0/-1 */
int at_exec_cmd(const char *cmd, uint32_t timeout_ms); /* cmd不带\r\n内部补, 返回AT_RespStatus */
PAT_Device at_get_device(void);                      /* 读resp_buf用 */
```

设计决定记录：
- 单设备单例（不学参考库处处传 ptDev）；resp 在 AT_Device 里，调用方经 at_get_device() 读
- resp_status 用枚举且负数=失败（比参考库 0/1 好）
- 指针别名 PAT_xxx 不用在有 const 诉求的接口上（`const PAT_PORT` 只能保护指针本身，保护不了表内容）
- 删掉了 BSD socket 全家桶（bind/listen/accept 是服务器才要的），module 层将来只暴露 net_connect_ap/net_connect/net_send/net_recv/net_close 5 个函数给 Paho transport 桥接

## 三、当前目录结构

```
APP/   app_test_task.c/h(旧协议帧消费者, 学习用, 任务已停用)
       app_uart_rx_task.c/h(旧协议生产者, 已停用)
BSP/   uart_ringbuf.c/h  uart_protocol.c/h  uart2_driver.c/h(物理层)
Middlewares/Third_Party/AT/   at_device.h(已定稿)
Core/  CubeMX生成(freertos.c 里任务创建在 USER CODE 区)
at_socket/  百问网参考库(不进编译)
```

## 四、已完成（含验收状态）

- [x] UART 链路: UART2+DMA循环+空闲中断+RingBuffer，**硬件回环验收通过**(rx=6="\r\nOK\r\n", err=0)
- [x] 自定义帧协议(学习目的，现停用)：proto_crc/proto_feed/proto_build + 队列收发闭环
- [x] AT-1 三层架构思路梳理（用户已理解，能答对"换模块动哪个文件"）
- [x] AT-3 头文件 at_device.h 定稿（用户手写, 经 3 轮 review 修正：enum语法/按值vs指针/const位置/接口收敛）
- [x] uart_task.c/h 从 APP 下沉到 BSP（git mv 完成, include 一行没改）
- [x] uart_task.c/h 重命名为 uart2_driver.c/h（名实对齐: 里面没有任何任务, 实为 UART2 硬件驱动; uvprojx/引用同步完成）
- [x] AT-4 port 层 at_port_uart2.c/h（用户手写; review 修正: 表初始化补'='、static原型移出头文件、函数先于表定义; 4道思考题已过）
- [x] AT-5 core 层 at_core.c（agent 写: 攒行+OK/ERROR/FAIL/'>'识别、resp拼接截断保护、锁+信号量一问一答、探活重试10×500ms; 未 include uart2_driver.h 铁律遵守）
- [x] AT-6 任务3 调用点集成（app_at_test + freertos.c 建任务, 用户写; **硬件验收通过**: 周期性 `AT -> 0, resp=[AT\r\n\r\nOK\r\n]`, 回显开启状态下非OK行只进缓冲不按门铃）
- [x] AT-7 core 加固·回显免疫（设计演进: 最初想把ATE0放module层 → 用户指出防线应归core → 加了at_set_echo → 用户再指出"core不认命令"铁律被违背 → **最终方案: 解析器回显免疫**(跳过与last_cmd相同的行, last_cmd由at_exec_cmd发送前记录) + 探活/ATE0 移交module层; at_set_echo已删, at_init只建资源不做I/O）
- [x] AT-8 module 层 esp8266.c（用户手写: esp8266_init(port)=at_init+探活重试+ATE0不判死+CWMODE; review 修正: i作用域/port注入/.h空参括号; **硬件验收通过**）
- [x] AT-9 修复 core 解析器 strstr 残留 bug（agent 的 bug: line[]跨行复用但从不补'\0', 空行继承上一行"OK"字节→strstr误匹配→门铃提前响→resp只剩"\r\n"但返回0; 修复: '\n'分支开头 line[len]='\0'。C经典三连坑: 缓冲区复用+未终止字符串+strstr）
- [x] AT-11 core 网络收发两把刀（agent 写: at_send_raw 裸发/at_net_recv 收网络环/解析任务+IPD分流状态机/g_net_rb+g_net_sem+丢弃计数; 家里机已上板: tcp_connect baidu.com:80 90ms连通, WiFi验收 IP=192.168.2.115）
- [x] AT-12 module 层 esp8266_send/recv + echo 联调（用户手写: CIPSEND→at_send_raw→等SEND OK 一条龙; recv=at_net_recv 透传; **硬件验收通过**: 板子与电脑 echo 服务器(8080端口)完成 TCP 双向对话 "hello net", +IPD 分流零污染, 令牌账本 2give/2take 平衡）
  - 设计勘误记录: CIPSEND **没有 OK 行**(前置应答只有'>'), 用户抓出 agent 时序图编造的"OK字节流"事件; exec 返回的 AT_RESP_OK 是 '>' 分支设置的枚举值; 信息源优先级=硬件抓包>官方文档>参考代码注释>泛化知识>画的图
- [x] AT-16 **"最后一厘米"下行闭环 + MQTTX 生态验证**（家里机: 用户手写 bsp_led/bsp_buzzer(极性封装在BSP, 查图三问: LED=PF9/PF10低电平亮/蜂鸣器PA7) + 命令格式升级"led on"/"buzzer on"(设备 动作); 硬件验收: MQTTX 下行控灯成功, hello 70+连发零掉线; 彩蛋: MQTTX 输入框误带回车→payload 多了 \r\n → 加仪表 print len 实锤——报文内容永远以字节为准）
  - MQTT 生态拓展(无板端改动): 手机 IoT MQTT Panel 订阅+开关控件 / Python paho-mqtt 消费脚本 / mosquitto_pub——broker 眼里人人平等, 设备端已就位, 观众席随意加
  - 顺带结论: 板子因杜邦线接 ESP8266 无法插回底板 → 底板蜂鸣器暂不可达, 用户明天在公司环境重新引线; BSP 接缝已就位(bsp_buzzer 换引脚只动一行)
  - 当前遗留作业: **命令表驱动重构**(else-if 链 → dev_cmd_t 查表分发, 切词+查表+handler; payload_in 非 NUL 结尾的坑已提示), 用户未交
- [x] AT-13 Paho MQTTPacket 库接入（agent 完成: clone eclipse/paho.mqtt.embedded-c → 挑客户端侧 11 文件入 Middlewares/Third_Party/PahoMQTT(+LICENSE/NOTICE) → Keil 新组+IncludePath+NOSTACKTRACE 宏; 该库只做报文打包/解包, 网络收发走调用方注入的 transport 函数指针）
- [x] AT-14 app_mqtt.c 完成（agent 按"用户明确委托+明天研究"代写: transport 三函数(用户已写)+CONNECT/SUBSCRIBE/主循环+broker序列, 全文标注四拍模式与学过的概念; WiFi凭证占位符化; AC5 揭出反序列化调用类型错误已按真实原型修复——VSCode IntelliSense 不报≠正确, Keil 全量编译才是裁决）
- [x] AT-15 **上板联调通过，项目通关**（2026-09-22 家里机: WiFi→IP→TCP→MQTT connected→suback qos0→hello N 连续30+条稳定上报→MQTTX 下行 rx:"on" 收到。彩蛋: MQTTX 输入带引号导致 LED 分支未触发——报文内容是字节, 解析前先看原样字节）
  - 接线关系: transport_getdata 经函数指针被 MQTTPacket_read 回调(同 AT_PORT 模式); sendPacketBuffer 由我们主动调(库只打包不发送)
  - 结构调整: app_mqtt_task 成为网络唯一主人(init/connect_ap/get_ip/tcp_connect(broker.emqx.io)+MQTT), at_test_task 已退役
- [x] AT-10 module 层 esp8266_connect_ap + esp8266_get_ip（用户手写, 公司环境 3 轮 review 通过: snprintf三态判定[n<0||n>=sizeof截断]/全路径return/字符vs字符串引号/**→&&/.h分号/strstr强转/const参数; **待硬件验证**(公司无机)）
  - 职责调整: CWMODE=1(2000ms) 从 init 挪进 connect_ap —— init 只管上电探活+ATE0, 谁连接谁负责自己的模式
  - get_ip 实际实现 `int esp8266_get_ip(PAT_Device pDev)`(内部 printf, 不拷给调用方), 用 AT+CIPSTA? 查 IP —— 与原作业签名(ip_buf,buf_len)不同但可用; 回家测试后若需要 IP 做他用再改造
  - 已知小尾巴: esp8266.c connect_ap 里 printf("%s", pDev->resp_buf) 需加 (char*) 强转, Keil 会有 signedness 警告

## 五、当前任务（下一步）

（AT-16 已完成：MQTT 下行控灯闭环 + MQTT 生态验证，见第四节。当前优先级：**表驱动重构作业 → 公司接线 → 下一模块**）

### 公司环境：接线
明天把核心板插回底板/重新引线接好 ESP8266（底板蜂鸣器随之恢复可达，bsp_buzzer 换引脚只动一行）。

### 遗留作业（优先级最高）：命令表驱动重构
else-if 链 → `dev_cmd_t` 查表分发：payload 按空格切词（dev + arg）→ 遍历 g_cmd_table → handler 执行。
验收：MQTTX 发 `led on` / `buzzer off`；此后新设备 = 表加一行 + 一个 handler，主循环永不再改。
注意：`payload_in` 非 NUL 结尾，处理前先按长度拷到局部数组（ strstr 坑的近亲）。

### 下一模块候选（表驱动完成后按兴趣选）
- **DHT11 温湿度上云**（单总线时序）→ Python paho-mqtt 订阅消费 → InfluxDB+Grafana 曲线面板
- OLED 状态面板（I2C：显示 IP/MQTT状态/温湿度）
- 红外遥控（定时器输入捕获 + NEC 解码，遥控器控灯）
- 断网缓存（SPI Flash：离线存数据联网补传——工业场景）
- **低频上报时的主动保活**（上报间隔接近 keepalive 时才需要；当前 5s 上报天然保活）

---

### 以下为已完成的任务记录（供参考）

### 任务 1：port 层 ——【已完成 2026-09-20，见第四节】

新建 `Middlewares/Third_Party/AT/at_port_uart2.c` + `at_port_uart2.h`（约 40 行）：

- .h：头文件保护 + `#include "at_device.h"` + `extern const AT_PORT at_port_stm32_uart2;`（**const 不能丢**，否则 .h/.c 类型不一致）
- .c：3 个 `static` 函数绑定 uart2_driver 原语 + 1 张 `const` 表
  - init → 调 `uart2_rx_start()`（它是 void 返回，port 的 init 返回 int，想想怎么补）
  - send → `uart2_send()`（返回值约定一致, 直接透传）
  - recv_byte → `uart2_receive_blocking()`（同上）

留给用户的 4 个思考题（写完要能回答）：
1. uart2 原语的返回值约定和 AT_PORT 要求对得上吗？
2. uart2_rx_start 是 void，init 怎么补返回值？
3. 3 个函数为什么必须 static？
4. 表的定义和 extern 声明里 const 分别放哪？

### 任务 2：core 层 ——【已完成 2026-09-20，at_core.c 已按下方规格实现】

重写 `Middlewares/Third_Party/AT/at_core.c`（旧版已删）：
- `at_parse_task`: 逐字节 `dev->port->recv_byte(&byte, 200)` → 攒行(line_buf 128) → 见'>'立即OK → 行尾追加进 resp_buf(截断保护) → strstr 判 OK/ERROR/FAIL → give at_sem → 清行
- `at_exec_cmd`: take at_lock → 清残留sem → 复位 resp/resp_len/resp_status → 去尾\r\n补齐发送 `dev->port->send()` → take at_sem(timeout) → 返回 resp_status → give at_lock
- `at_init`: 校验port非空 → 建mutex+binary sem → port->init可调可不调 → xTaskCreate at_parse_task(栈512) → 探活 "AT" 重试10次×500ms
- 严禁 include uart2_driver.h——core 只认 dev->port->

### 任务 3：调用点 ——【已完成 2026-09-20，硬件验收通过】

`APP/app_test_task.c` 新建 app_at_test 任务: `at_init(&at_port_stm32_uart2)` → 循环 `at_exec_cmd("AT",1000)` 打印结果 + g_uart2_* 统计；`Core/Src/freertos.c` USER CODE 区创建任务(栈512)。

### 任务 4：硬件验证清单（到 Keil 环境）

1. Keil: Manage Project Items 加组 `Middlewares/AT`，把 at_core.c/at_port_uart2.c 加进去
2. Keil: uart_task.c 从 APP 组移到 BSP 组——**已完成**(uvprojx 已改为 BSP\uart2_driver.c 并补进 uart_protocol.c, IncludePath 已加 ..\Middlewares\Third_Party\AT, 重开 Keil 验证编译即可)
3. Keil: Include Paths 加 `..\Middlewares\Third_Party\AT`
4. .vscode/c_cpp_properties.json includePath 同步加
5. 编译烧录，预期串口1输出: AT init OK → 周期性 `<< OK, resp=[AT\r\n\r\nOK\r\n]`

## 六、后续里程碑

- **AT-5 WiFi（代码完, 待硬件验证）**: esp8266_connect_ap/get_ip 已过 review（见第五节回家第一棒）；esp8266_init 已验收
- **AT-6 TCP**: esp8266_tcp_connect（见第五节回家第二棒）→ esp8266_send('>' 流程 + core 加 at_send_raw) → esp8266_recv(+IPD 分流, core 层新逻辑)
- **AT-7 Paho MQTT**: 移植 paho.mqtt.embedded-c 的 MQTTPacket, 桥接 transport_sendPacketBuffer/transport_getdata/Timer 四函数到 module 层 5 个 net_xxx
- 老规矩：每个里程碑"用户写关键代码 + agent review"，硬件验收后 commit push

## 七、协作约定（对 agent 的硬性要求，用户明确反馈过）

1. **严禁一口气替用户把代码全部写完**：每次一个小目标，关键代码用户自己写并贴回来 review
2. 写错了先指出问题+讲为什么，让用户自己改；用户求助(明确说"帮我改")才可以代写
3. 教参数设计用"一句话数名词"法：一句话说清谁干什么，名词=参数
4. 代码注释用中文，写"给三个月后的自己看"的注释，不留对话考古痕迹
5. 文件结构: BSP=硬件驱动 / APP=业务任务 / Middlewares/Third_Party/AT=AT协议栈（平铺, 无Inc/Src子目录）

## 八、环境备忘

- Git 推拉需要代理软件(7890)运行; 仓库: github.com/Leo-ew-huang/STM32F407_IoT_Gateway（双机同步）
- 编译环境在**家里的机器**（Keil+MicroLIB+硬件）；公司机器只有 VSCode 写代码
- UART1=printf调试(fputc重定向), UART2=ESP8266(DMA必须循环模式, 中断上下文禁止阻塞)
