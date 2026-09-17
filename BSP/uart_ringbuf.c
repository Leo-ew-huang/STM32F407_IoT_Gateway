#include "uart_ringbuf.h"

// 初始化：把缓冲区清空，头尾指针归零
void rb_init(struct RingBuffer *rb)
{
    rb->head = 0;
    rb->tail = 0;
}

// 写入一个字节（生产者用）：满了返回失败（或覆盖）
int  rb_write(struct RingBuffer *rb, uint8_t data)
{
    int next = (rb->head + 1) % RING_BUFFER_SIZE;
    if (next == rb->tail) {
        // 缓冲区满了，返回失败
        return -1;
    }
    rb->buffer[rb->head] = data;
    rb->head = next;
    return 0;
}

// 读出一个字节（消费者用）：空返回失败
int  rb_read(struct RingBuffer *rb, uint8_t *data)
{
    if (rb->head == rb->tail) {
        // 缓冲区空了，返回失败
        return -1;
    }
    *data = rb->buffer[rb->tail];
    rb->tail = (rb->tail + 1) % RING_BUFFER_SIZE;
    return 0;
}

// 查询当前已存多少字节（没读完的）
int  rb_used(const struct RingBuffer *rb)
{
    return (rb->head - rb->tail + RING_BUFFER_SIZE) % RING_BUFFER_SIZE;
}
