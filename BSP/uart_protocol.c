#include "uart_protocol.h"

uint8_t proto_crc(const uint8_t *cmd_ptr, uint8_t len)
{
    uint8_t crc = 0;
    /* cmd_ptr 指向 CMD 字节：cmd_ptr[0]=CMD, [1]=LEN, [2..len+1]=DATA
       所以 CMD + LEN + DATA 共 (len+2) 个字节一起做 XOR */
    for (uint8_t i = 0; i < (len + 2); i++)
        crc ^= cmd_ptr[i];
    return crc;
}

int proto_feed(ProtoParser *p, uint8_t byte)
{
    switch (p->state)
    {
    case ST_WAIT_HEAD:
        /* 只有 0xAA 才是帧头，否则丢弃噪声，停在原地继续等 */
        if (byte == PROTO_HEAD)
            p->state = ST_GET_CMD;
        break;

    case ST_GET_CMD:
        /* CMD 是开放的，任何字节都可作命令，不必局限于已知枚举 */
        p->frame.cmd = byte;
        p->state = ST_GET_LEN;
        break;

    case ST_GET_LEN:
        if (byte > PROTO_DATA_MAX)
        {
            /* 长度越界 -> 半帧作废，回找帧头 */
            p->state = ST_WAIT_HEAD;
            if (byte == PROTO_HEAD)     /* 当前字节可能是新帧头，别丢 */
                p->state = ST_GET_CMD;
        }
        else
        {
            p->frame.len = byte;
            p->data_idx = 0;            /* 准备逐字节收 DATA */
            p->state = (byte == 0) ? ST_GET_CRC : ST_GET_DATA;
        }
        break;

    case ST_GET_DATA:
        p->frame.data[p->data_idx++] = byte;
        if (p->data_idx >= p->frame.len)
            p->state = ST_GET_CRC;
        break;

    case ST_GET_CRC:
        /* frame 的 cmd/len/data 在内存连续，可传 &frame.cmd 直接扫 CMD..DATA */
        if (byte == proto_crc(&p->frame.cmd, p->frame.len))
            p->state = ST_GET_TAIL;
        else
        {
            p->state = ST_WAIT_HEAD;
            if (byte == PROTO_HEAD)
                p->state = ST_GET_CMD;
        }
        break;

    case ST_GET_TAIL:
        if (byte == PROTO_TAIL)
        {
            p->state = ST_WAIT_HEAD;    /* 复位，等下一帧 */
            return 1;                   /* 凑齐一整帧，结果在 p->frame */
        }
        else
        {
            p->state = ST_WAIT_HEAD;
            if (byte == PROTO_HEAD)
                p->state = ST_GET_CMD;
        }
        break;
    }
    return 0;
}

int proto_build(const ProtoFrame *frame, uint8_t *buffer, uint8_t buffer_size)
{
    uint8_t frame_size;

    if (frame == 0 || buffer == 0 || frame->len > PROTO_DATA_MAX)
        return -1;

    frame_size = (uint8_t)(frame->len + 5);
    if (buffer_size < frame_size)
        return -1;

    buffer[0] = PROTO_HEAD;
    buffer[1] = frame->cmd;
    buffer[2] = frame->len;

    for (uint8_t i = 0; i < frame->len; i++)
        buffer[3 + i] = frame->data[i];

    buffer[3 + frame->len] = proto_crc(&buffer[1], frame->len);
    buffer[4 + frame->len] = PROTO_TAIL;

    return frame_size;
}
