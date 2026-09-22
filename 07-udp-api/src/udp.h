#ifndef UDP_H
#define UDP_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define UDP_MAX_PAYLOAD 1472

/* 应用接口的 IP、端口使用主机字节序；fd 只是本库中的槽位编号。 */
struct udp_address {
    uint32_t ip;
    uint16_t port;
};

int udp_socket(void);
int udp_bind(int fd, uint16_t port);

/* 非阻塞：成功表示复制进发送队列，不表示对端已经收到。 */
ssize_t udp_sendto(int fd, const void *data, size_t length, struct udp_address peer);

/* 一次取一个数据报；缓冲区太小时截断，剩余部分丢弃；队列空返回 -1/EAGAIN。 */
ssize_t udp_recvfrom(int fd, void *data, size_t capacity, struct udp_address *peer);

/* 取走最近一次异步发送错误；0 表示没有错误；多个错误可能合并。 */
int udp_take_error(int fd);
int udp_close(int fd);

#endif
