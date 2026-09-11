# Handoff：STM32F407 + ESP8266 UART协议实战

## 一、我的当前情况

我目前在学习嵌入式单片机开发，已经学习完成：

- STM32 基础
- FreeRTOS
- MQTT
- WebServer
- UART 等基础外设

我的目标方向是：

> 嵌入式单片机 / BSP 软件开发

目前手里的硬件：

- STM32F407ZGT6
- ESP8266
- 韦东山瑞士军刀开发板
- 开发板上还有一些常见传感器和外设模块

我的 STM32F407ZGT6 已经使用 UART2 外接 ESP8266。

---

# 二、目前想做的事情

我现在不想继续单独学习知识点，而是想通过一个完整的开源项目/实战项目，把已经学习过的：

```text
STM32
+
FreeRTOS
+
UART
+
DMA
+
通信协议
+
ESP8266
+
MQTT
+
WebServer
```

真正组合起来。

计划先参考 GitHub 上的 STM32 + ESP8266 + FreeRTOS + MQTT 项目，但不要直接照抄，而是：

```text
阅读开源项目
    ↓
理解架构
    ↓
自己重新设计
    ↓
移植到 STM32F407
    ↓
逐步增加自己的功能
```

其中一个参考项目：

https://github.com/fighterCK/ESP8266

这个项目包含：

- STM32
- ESP8266
- FreeRTOS
- MQTT
- DHT11
- OLED
- 多任务

可以作为架构和实现思路的参考，但最终代码要尽量自己实现。

---

# 三、当前第一阶段：先实现 UART 自定义通信协议

目前我们决定：

> 暂时不要急着做 MQTT 和 WebServer。

先把 STM32F407 和 ESP8266 之间的 UART 通信基础打牢。

目标架构：

```text
STM32F407
    │
    │ UART2
    ▼
ESP8266
```

STM32 端：

```text
UART2
 ↓
DMA
 ↓
RingBuffer
 ↓
UART接收Task
 ↓
协议状态机
 ↓
CRC校验
 ↓
解析 CMD + DATA
 ↓
FreeRTOS Queue
 ↓
Application Task
```

希望通过这个项目真正理解：

- UART 为什么需要通信协议
- 数据帧设计
- 帧头/帧尾
- CMD
- LEN
- DATA
- CRC
- DMA 接收
- RingBuffer
- 状态机解析
- FreeRTOS Queue
- 通信异常处理

---

# 四、第一版 UART 协议

暂时采用一个简单协议，不要一开始设计得太复杂。

数据帧：

```text
┌──────┬─────┬─────┬──────────┬─────┬──────┐
│ HEAD │ CMD │ LEN │   DATA   │ CRC │ TAIL │
└──────┴─────┴─────┴──────────┴─────┴──────┘
  1B     1B    1B      N B      1B    1B
```

定义：

```text
HEAD = 0xAA
TAIL = 0x55
```

字段：

```text
HEAD：帧头
CMD ：命令
LEN ：DATA长度
DATA：实际数据
CRC ：校验
TAIL：帧尾
```

例如：

```text
AA 01 02 01 09 0B 55
```

表示：

```text
AA       帧头
01       CMD = 温度
02       DATA长度
01 09    DATA = 265，也就是 26.5℃
0B       CRC
55       帧尾
```

第一版 CRC 可以先使用简单的 XOR 校验：

```c
CRC = CMD ^ LEN ^ DATA[0] ^ DATA[1] ...
```

后续再升级到：

```text
CRC8
CRC16
```

---

# 五、第一版 CMD 设计

暂时可以采用：

```text
0x01 → 温度
0x02 → 湿度
0x03 → 光照
0x04 → LED控制
0x05 → 蜂鸣器控制
0x06 → 获取设备状态
0x07 → 设置采样周期
```

后续根据实际项目继续扩展。

例如：

STM32 → ESP8266：

```text
AA 01 02 01 09 CRC 55
```

表示：

> STM32发送温度 26.5℃

ESP8266 → STM32：

```text
AA 04 01 01 CRC 55
```

表示：

> ESP8266要求 STM32 打开 LED

---

# 六、重点：协议解析状态机

UART 本质上是连续字节流，不能假设一次 UART 接收就刚好得到完整的一帧。

例如：

```text
AA 01 02 01 09 CRC 55
```

可能分多次收到：

```text
AA
01 02
01
09 CRC
55
```

也可能一次收到多帧：

```text
AA 01 02 01 09 CRC 55 AA 04 01 01 CRC 55
```

因此 STM32 端需要设计状态机：

```text
WAIT_HEAD
    ↓
GET_CMD
    ↓
GET_LEN
    ↓
GET_DATA
    ↓
GET_CRC
    ↓
GET_TAIL
    ↓
FRAME_OK
```

收到无效数据时能够重新寻找：

```text
0xAA
```

例如：

```text
12 34 56 78 AA 01 02 01 09 CRC 55
```

应该自动丢弃前面的垃圾数据，然后从 `AA` 开始解析。

---

# 七、计划实现的代码模块

建议最终 STM32 工程增加：

```text
uart_protocol.h
uart_protocol.c

uart_ringbuffer.h
uart_ringbuffer.c

uart_task.h
uart_task.c
```

职责：

### uart_ringbuffer

负责：

```text
UART DMA收到的数据
        ↓
RingBuffer
```

### uart_protocol

负责：

```text
字节流
 ↓
协议状态机
 ↓
完整Frame
 ↓
CRC校验
 ↓
解析CMD/DATA
```

### uart_task

负责：

```text
从RingBuffer读取数据
 ↓
调用协议解析器
 ↓
收到完整消息
 ↓
发送FreeRTOS Queue
```

---

# 八、希望采用的最终架构

```text
                    UART2
                      │
                      ▼
                    DMA
                      │
                      ▼
                RingBuffer
                      │
                      ▼
                 UART Task
                      │
                      ▼
              Protocol Parser
                      │
              ┌───────┴───────┐
              │               │
           CRC OK          CRC ERROR
              │               │
              ▼               ▼
            Queue            丢弃
              │
              ▼
       Application Task
```

以后再加入：

```text
MQTT
WebServer
Sensor
Display
Alarm
```

最终形成：

```text
                 MQTT Server
                      │
                      │ WiFi
                      ▼
                  ESP8266
                      │
                    UART2
                      │
                      ▼
               STM32F407
                      │
                  FreeRTOS
                      │
       ┌──────────────┼──────────────┐
       ↓              ↓              ↓
 SensorTask       MqttTask       WebTask
       │              │              │
       ↓              ↓              ↓
   Sensor数据       MQTT         WebServer
```

---

# 九、后续完整项目目标

第一阶段：

```text
UART协议
+
DMA
+
RingBuffer
+
状态机
+
CRC
+
Queue
```

第二阶段：

```text
STM32F407
+
ESP8266
+
UART协议
+
MQTT
```

第三阶段：

```text
STM32F407
+
ESP8266
+
FreeRTOS
+
MQTT
+
WebServer
+
传感器
+
OLED
+
按键
+
LED
+
蜂鸣器
```

第四阶段继续增加：

```text
ACK
超时
重传
心跳
错误码
设备状态
参数配置
OTA
```

---

# 十、对 Agent 的要求

请不要直接给我一大坨完整代码让我复制。

我的编程基础和编程思维目前比较薄弱，我希望通过这个项目提升真正的嵌入式工程能力。

请采用“带着我做项目”的方式：

1. 每次只推进一个明确的小目标。
2. 先解释为什么这么设计，再写代码。
3. 尽量让我自己实现关键代码。
4. 可以给出代码框架，但不要让我无脑复制。
5. 如果我写错了，先指出问题和原因，再让我修改。
6. 解释关键 API，例如：
   - HAL_UART
   - DMA
   - FreeRTOS Queue
   - Task
   - RingBuffer
7. 每完成一个模块，带我做测试。
8. 不要一次把整个项目全部实现完。
9. 逐步提高难度。
10. 最终让我能够自己解释整个项目的架构和代码。

---

# 十一、第一步应该做什么

请先不要开始写代码。

先让我提供当前 STM32F407 工程，然后：

1. 分析我的工程结构
2. 确认我使用的是 HAL 还是标准库
3. 确认 FreeRTOS 版本和配置
4. 检查 UART2 当前配置
5. 检查 DMA 配置
6. 检查中断配置
7. 确认当前工程能够正常运行
8. 然后我们再开始设计并实现第一版 UART 协议

如果我的工程配置和之前描述的不一致，以我的实际工程为准。

最终目标不是“把代码跑起来”，而是：

> **让我真正理解并亲手实现 STM32F407 ↔ ESP8266 的可靠 UART 通信。**