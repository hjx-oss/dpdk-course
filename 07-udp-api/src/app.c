#include "app.h"
#include "udp.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int sockets[3] = {-1, -1, -1};

int app_init(void)
{
    for (unsigned index = 0; index < 3; index++) {
        sockets[index] = udp_socket();
        if (sockets[index] < 0 || udp_bind(sockets[index], 9000 + index) < 0) {
            app_close();
            return -1;
        }
    }
    /* 不等外部请求，先主动发送；地址为主机字节序的 192.0.2.1。 */
    struct udp_address peer = {0xc0000201, 40001};
    const char greeting[] = "hello-from-dpdk";
    if (udp_sendto(sockets[2], greeting, sizeof(greeting) - 1, peer) < 0) {
        app_close();
        return -1;
    }
    puts("APP send queued: 9002 -> 192.0.2.1:40001 hello-from-dpdk");
    return 0;
}

void app_poll(void)
{
    for (unsigned index = 0; index < 3; index++) {
        int fd = sockets[index];
        int error = udp_take_error(fd);
        if (error > 0) {
            printf("APP async error: port=%u error=%s\n", 9000 + index, strerror(error));
        }
        /* 每轮每端口最多处理八条，处理完就交还主循环。 */
        for (unsigned count = 0; count < 8; count++) {
            uint8_t data[UDP_MAX_PAYLOAD];
            struct udp_address peer;
            ssize_t length = udp_recvfrom(fd, data, sizeof(data), &peer);
            if (length < 0) {
                break;
            }
            if (index == 2) {
                printf("APP active received: %.*s\n", (int)length, (char *)data);
                continue;
            }
            if (index == 1) {
                for (ssize_t i = 0; i < length; i++) {
                    if (data[i] >= 'a' && data[i] <= 'z') {
                        data[i] = data[i] - 'a' + 'A';
                    }
                }
            }
            if (udp_sendto(fd, data, (size_t)length, peer) < 0) {
                printf("APP reply rejected: port=%u error=%s\n",
                       9000 + index,
                       strerror(errno));
            }
        }
    }
}

void app_close(void)
{
    for (unsigned index = 0; index < 3; index++) {
        if (sockets[index] >= 0) {
            udp_close(sockets[index]);
            sockets[index] = -1;
        }
    }
}
