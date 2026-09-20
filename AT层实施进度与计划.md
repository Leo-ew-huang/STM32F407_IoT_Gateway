# AT 层实施进度与计划（跨环境 Handoff 文档）

> 本文档供两台工作环境（公司/家）之间同步进度使用，也给下一个接手的 AI agent。
> 最后更新：2026-09-20（公司环境）

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
物理层    BSP/uart_task.c(已完成) DMA→暂存数组→RingBuffer→阻塞读字节, 硬件验收过
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
BSP/   uart_ringbuf.c/h  uart_protocol.c/h  uart_task.c/h(物理层)
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

## 五、当前任务（下一步，用户回家后从这里继续）

### 任务 1：port 层 —— 【用户自己写，严禁 agent 代写】

新建 `Middlewares/AT/at_port_uart2.c` + `at_port_uart2.h`（约 40 行）：

- .h：头文件保护 + `#include "at_device.h"` + `extern const AT_PORT at_port_stm32_uart2;`（**const 不能丢**，否则 .h/.c 类型不一致）
- .c：3 个 `static` 函数绑定 uart_task 原语 + 1 张 `const` 表
  - init → 调 `uart2_rx_start()`（它是 void 返回，port 的 init 返回 int，想想怎么补）
  - send → `uart2_send()`（返回值约定一致, 直接透传）
  - recv_byte → `uart2_receive_blocking()`（同上）

留给用户的 4 个思考题（写完要能回答）：
1. uart2 原语的返回值约定和 AT_PORT 要求对得上吗？
2. uart2_rx_start 是 void，init 怎么补返回值？
3. 3 个函数为什么必须 static？
4. 表的定义和 extern 声明里 const 分别放哪？

### 任务 2：core 层 —— 【agent 写（机械性重构）】

重写 `Middlewares/AT/at_core.c`（旧版已删）：
- `at_parse_task`: 逐字节 `dev->port->recv_byte(&byte, 200)` → 攒行(line_buf 128) → 见'>'立即OK → 行尾追加进 resp_buf(截断保护) → strstr 判 OK/ERROR/FAIL → give at_sem → 清行
- `at_exec_cmd`: take at_lock → 清残留sem → 复位 resp/resp_len/resp_status → 去尾\r\n补齐发送 `dev->port->send()` → take at_sem(timeout) → 返回 resp_status → give at_lock
- `at_init`: 校验port非空 → 建mutex+binary sem → port->init可调可不调 → xTaskCreate at_parse_task(栈512) → 探活 "AT" 重试10次×500ms
- 严禁 include uart_task.h——core 只认 dev->port->

### 任务 3：调用点 —— 【用户写】

`APP/app_test_task.c` 新建 app_at_test 任务: `at_init(&at_port_stm32_uart2)` → 循环 `at_exec_cmd("AT",1000)` 打印结果 + g_uart2_* 统计；`Core/Src/freertos.c` USER CODE 区创建任务(栈512)。

### 任务 4：硬件验证清单（到 Keil 环境）

1. Keil: Manage Project Items 加组 `Middlewares/AT`，把 at_core.c/at_port_uart2.c 加进去
2. Keil: uart_task.c 从 APP 组移到 BSP 组（文件已 git mv，不移会报找不到文件）
3. Keil: Include Paths 加 `..\Middlewares\Third_Party\AT`
4. .vscode/c_cpp_properties.json includePath 同步加
5. 编译烧录，预期串口1输出: AT init OK → 周期性 `<< OK, resp=[AT\r\n\r\nOK\r\n]`

## 六、后续里程碑

- **AT-5 WiFi**: esp8266.c(module 层, 引导用户写): AT+CWMODE=1 → AT+CWJAP(10s超时) → AT+CIFSR 查IP
- **AT-6 TCP**: AT+CIPMUX=0 → AT+CIPSTART="TCP","host",port → AT+CIPSEND(注意'>'提示符特判) → +IPD 分流(解析任务里识别"+IPD,<len>:"不算命令应答)
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
