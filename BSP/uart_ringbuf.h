#ifndef UART_RINGBUF_H
#define UART_RINGBUF_H

#define RING_BUFFER_SIZE 256


#include <stdint.h>

struct RingBuffer {
    uint8_t buffer[RING_BUFFER_SIZE];  // 缓冲区
    int head;     // 写入位置
    int tail;     // 读取位置
};


// 初始化：把缓冲区清空，头尾指针归零
void rb_init(RingBuffer *rb);

// 写入一个字节（生产者用）：满了返回失败（或覆盖）
int  rb_write(RingBuffer *rb, uint8_t data);

// 读出一个字节（消费者用）：空返回失败
int  rb_read(RingBuffer *rb, uint8_t *data);

// 查询当前已存多少字节（没读完的）
int  rb_used(const RingBuffer *rb);

#endif /* UART_RINGBUF_H */