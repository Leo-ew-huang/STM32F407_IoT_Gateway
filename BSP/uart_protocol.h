#ifndef UART_PROTOCOL_H
#define UART_PROTOCOL_H

#include <stdint.h>

/* 帧边界 */
#define PROTO_HEAD        0xAA
#define PROTO_TAIL        0x55
#define PROTO_DATA_MAX    8        /* 单帧 DATA 最大长度，按需定 */

/* 命令枚举（沿用 Handoff 五章的 CMD 设计） */
typedef enum {
    CMD_TEMPERATURE = 0x01,  /* 温度 */
    CMD_HUMIDITY    = 0x02,  /* 湿度 */
    CMD_LIGHT       = 0x03,  /* 光照 */
    CMD_LED_CTRL    = 0x04,  /* LED 控制 */
    CMD_BUZZER      = 0x05,  /* 蜂鸣器 */
    CMD_GET_STATUS  = 0x06,  /* 查设备状态 */
    CMD_SET_PERIOD  = 0x07,  /* 设采样周期 */
} CmdType;

/* 解析完的完整一帧（存这个结构，等后面发队列用） */
typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[PROTO_DATA_MAX];
} ProtoFrame;

typedef enum {
    ST_WAIT_HEAD = 0x01,
    ST_GET_CMD   = 0x02,
    ST_GET_LEN   = 0x03,
    ST_GET_DATA  = 0x04,
    ST_GET_CRC   = 0x05,
    ST_GET_TAIL  = 0x06,
} ProtoState;

typedef struct {
    ProtoState state;     /* 当前状态 */
    ProtoFrame frame;     /* 正在拼接的帧 */
    uint8_t data_idx;     /* 已收了几个 DATA 字节 */
} ProtoParser;

/* XOR 校验：cmd_ptr 指向帧内 CMD 字节，len 为 DATA 长度 */
uint8_t proto_crc(const uint8_t *cmd_ptr, uint8_t len);

/* 逐字节喂入，返回 0 表示还没凑够一帧；返回 1 表示凑齐了一整帧(结果在 parser->frame) */
int proto_feed(ProtoParser *p, uint8_t byte);

/* 组装完整帧，成功返回帧长度，失败返回 -1 */
int proto_build(const ProtoFrame *frame, uint8_t *buffer, uint8_t buffer_size);


#endif /* UART_PROTOCOL_H */