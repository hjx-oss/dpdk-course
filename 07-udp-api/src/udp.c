#include "udp.h"
#include "arp.h"
#include "net_config.h"
#include "udp_internal.h"

#include <errno.h>
#include <rte_cycles.h>
#include <rte_udp.h>
#include <string.h>

#define SOCKET_COUNT 4
#define QUEUE_CAPACITY 8
#define SEND_TIMEOUT_SECONDS 4

struct datagram {
    struct udp_address peer;
    uint16_t length;
    uint64_t deadline;
    uint8_t data[UDP_MAX_PAYLOAD];
};

struct datagram_queue {
    struct datagram slots[QUEUE_CAPACITY];
    unsigned head;
    unsigned count;
};

struct udp_endpoint {
    bool used;
    uint16_t port;
    int error;
    struct datagram_queue rx;
    struct datagram_queue tx;
};

static struct udp_endpoint endpoints[SOCKET_COUNT];
static struct rte_mempool *packet_pool;
static struct rte_ether_addr local_mac;
static unsigned next_endpoint;

static struct udp_endpoint *endpoint(int fd)
{
    if (fd < 0 || fd >= SOCKET_COUNT || !endpoints[fd].used) {
        errno = EBADF;
        return NULL;
    }
    return &endpoints[fd];
}

static void pop(struct datagram_queue *queue)
{
    queue->head = (queue->head + 1) % QUEUE_CAPACITY;
    queue->count--;
}

void udp_init(struct rte_mempool *pool, const struct rte_ether_addr *mac)
{
    memset(endpoints, 0, sizeof(endpoints));
    packet_pool = pool;
    local_mac = *mac;
    next_endpoint = 0;
}

int udp_socket(void)
{
    for (int fd = 0; fd < SOCKET_COUNT; fd++) {
        if (!endpoints[fd].used) {
            memset(&endpoints[fd], 0, sizeof(endpoints[fd]));
            endpoints[fd].used = true;
            return fd;
        }
    }
    errno = EMFILE;
    return -1;
}

int udp_bind(int fd, uint16_t port)
{
    struct udp_endpoint *socket = endpoint(fd);
    if (socket == NULL) {
        return -1;
    }
    if (port == 0 || socket->port != 0) {
        errno = EINVAL;
        return -1;
    }
    for (unsigned index = 0; index < SOCKET_COUNT; index++) {
        if (endpoints[index].used && endpoints[index].port == port) {
            errno = EADDRINUSE;
            return -1;
        }
    }
    socket->port = port;
    return 0;
}

ssize_t udp_sendto(int fd, const void *data, size_t length, struct udp_address peer)
{
    struct udp_endpoint *socket = endpoint(fd);
    if (socket == NULL) {
        return -1;
    }
    if (socket->port == 0 || peer.port == 0 || (length > 0 && data == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (length > UDP_MAX_PAYLOAD) {
        errno = EMSGSIZE;
        return -1;
    }
    uint32_t host = peer.ip & ~NETMASK;
    if ((peer.ip & NETMASK) != (SERVER_IP & NETMASK) || host == 0 || host == ~NETMASK ||
        peer.ip == SERVER_IP) {
        errno = ENETUNREACH;
        return -1;
    }
    struct datagram_queue *queue = &socket->tx;
    if (queue->count == QUEUE_CAPACITY) {
        errno = EAGAIN;
        return -1;
    }
    unsigned tail = (queue->head + queue->count) % QUEUE_CAPACITY;
    struct datagram *message = &queue->slots[tail];
    message->peer = peer;
    message->length = (uint16_t)length;
    message->deadline =
        rte_get_timer_cycles() + SEND_TIMEOUT_SECONDS * rte_get_timer_hz();
    if (length > 0) {
        memcpy(message->data, data, length);
    }
    queue->count++;
    return (ssize_t)length;
}

ssize_t udp_recvfrom(int fd, void *data, size_t capacity, struct udp_address *peer)
{
    struct udp_endpoint *socket = endpoint(fd);
    if (socket == NULL) {
        return -1;
    }
    if (capacity > 0 && data == NULL) {
        errno = EINVAL;
        return -1;
    }
    struct datagram_queue *queue = &socket->rx;
    if (queue->count == 0) {
        errno = EAGAIN;
        return -1;
    }
    struct datagram *message = &queue->slots[queue->head];
    size_t copied = message->length < capacity ? message->length : capacity;
    if (copied > 0) {
        memcpy(data, message->data, copied);
    }
    if (peer != NULL) {
        *peer = message->peer;
    }
    pop(queue);
    return (ssize_t)copied;
}

void udp_report_error(int fd, int error)
{
    struct udp_endpoint *socket = endpoint(fd);
    if (socket != NULL) {
        socket->error = error;
    }
}

int udp_take_error(int fd)
{
    struct udp_endpoint *socket = endpoint(fd);
    if (socket == NULL) {
        return -1;
    }
    int error = socket->error;
    socket->error = 0;
    return error;
}

int udp_close(int fd)
{
    struct udp_endpoint *socket = endpoint(fd);
    if (socket == NULL) {
        return -1;
    }
    /* 队列内是值副本，没有尚未交给驱动的 mbuf。 */
    memset(socket, 0, sizeof(*socket));
    return 0;
}

bool udp_input(struct rte_mbuf *packet, struct rte_ipv4_hdr *ip)
{
    (void)packet;
    uint16_t ip_length = rte_be_to_cpu_16(ip->total_length);
    if (ip_length < sizeof(*ip) + sizeof(struct rte_udp_hdr)) {
        return false;
    }
    struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip + 1);
    uint16_t udp_length = rte_be_to_cpu_16(udp->dgram_len);
    if (udp_length < sizeof(*udp) || udp_length != ip_length - sizeof(*ip) ||
        udp_length - sizeof(*udp) > UDP_MAX_PAYLOAD) {
        return false;
    }
    if (udp->dgram_cksum != 0 && rte_ipv4_udptcp_cksum_verify(ip, udp) != 0) {
        return false;
    }
    uint16_t port = rte_be_to_cpu_16(udp->dst_port);
    for (unsigned index = 0; index < SOCKET_COUNT; index++) {
        struct udp_endpoint *socket = &endpoints[index];
        if (!socket->used || socket->port == 0 || socket->port != port) {
            continue;
        }
        struct datagram_queue *queue = &socket->rx;
        if (queue->count == QUEUE_CAPACITY) {
            return false;
        }
        unsigned tail = (queue->head + queue->count) % QUEUE_CAPACITY;
        struct datagram *message = &queue->slots[tail];
        message->peer.ip = rte_be_to_cpu_32(ip->src_addr);
        message->peer.port = rte_be_to_cpu_16(udp->src_port);
        message->length = udp_length - sizeof(*udp);
        memcpy(message->data, udp + 1, message->length);
        queue->count++;
        return true;
    }
    return false;
}

static struct rte_mbuf *make_packet(uint16_t source_port,
                                    const struct datagram *message,
                                    const struct rte_ether_addr *destination)
{
    struct rte_mbuf *packet = rte_pktmbuf_alloc(packet_pool);
    if (packet == NULL) {
        return NULL;
    }
    uint16_t ip_length =
        sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + message->length;
    uint16_t frame_length = sizeof(struct rte_ether_hdr) + ip_length;
    if (frame_length < RTE_ETHER_MIN_LEN - RTE_ETHER_CRC_LEN) {
        frame_length = RTE_ETHER_MIN_LEN - RTE_ETHER_CRC_LEN;
    }
    char *data = rte_pktmbuf_append(packet, frame_length);
    if (data == NULL) {
        rte_pktmbuf_free(packet);
        return NULL;
    }
    memset(data, 0, frame_length);
    struct rte_ether_hdr *ethernet = (struct rte_ether_hdr *)data;
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(ethernet + 1);
    struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip + 1);
    ethernet->src_addr = local_mac;
    ethernet->dst_addr = *destination;
    ethernet->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    ip->version_ihl = 0x45;
    ip->total_length = rte_cpu_to_be_16(ip_length);
    ip->time_to_live = 64;
    ip->next_proto_id = IPPROTO_UDP;
    ip->src_addr = rte_cpu_to_be_32(SERVER_IP);
    ip->dst_addr = rte_cpu_to_be_32(message->peer.ip);
    udp->src_port = rte_cpu_to_be_16(source_port);
    udp->dst_port = rte_cpu_to_be_16(message->peer.port);
    udp->dgram_len = rte_cpu_to_be_16(sizeof(*udp) + message->length);
    memcpy(udp + 1, message->data, message->length);
    ip->hdr_checksum = rte_ipv4_cksum(ip);
    udp->dgram_cksum = rte_ipv4_udptcp_cksum(ip, udp);
    return packet;
}

struct rte_mbuf *udp_poll(uint64_t now, int *owner)
{
    for (unsigned count = 0; count < SOCKET_COUNT; count++) {
        unsigned index = next_endpoint;
        next_endpoint = (next_endpoint + 1) % SOCKET_COUNT;
        struct udp_endpoint *socket = &endpoints[index];
        struct datagram_queue *queue = &socket->tx;
        if (!socket->used || queue->count == 0) {
            continue;
        }
        struct datagram *message = &queue->slots[queue->head];
        struct rte_ether_addr destination;
        enum arp_state state =
            arp_resolve(rte_cpu_to_be_32(message->peer.ip), &destination, now);
        if (state == ARP_FAILED || now >= message->deadline) {
            socket->error = EHOSTUNREACH;
            pop(queue);
            continue;
        }
        if (state != ARP_REACHABLE) {
            continue;
        }
        struct rte_mbuf *packet = make_packet(socket->port, message, &destination);
        pop(queue);
        if (packet == NULL) {
            socket->error = ENOBUFS;
            continue;
        }
        *owner = (int)index;
        return packet;
    }
    return NULL;
}

void udp_shutdown(void)
{
    /* 退出时丢弃尚未收取或发出的数据，释放全部逻辑槽位。 */
    memset(endpoints, 0, sizeof(endpoints));
}
