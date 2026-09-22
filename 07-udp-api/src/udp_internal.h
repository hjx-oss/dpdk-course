#ifndef UDP_INTERNAL_H
#define UDP_INTERNAL_H

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <stdbool.h>

/* 本课所有接口都由同一个 lcore 顺序调用，不支持并发访问。 */
void udp_init(struct rte_mempool *pool, const struct rte_ether_addr *mac);

/* 只复制有效载荷，不接管收到的 mbuf；IPv4 已检查格式和长度。 */
bool udp_input(struct rte_mbuf *packet, struct rte_ipv4_hdr *ip);

/* 每次最多交出一个待发 mbuf；调用者负责 TX，未被接收时负责释放。 */
struct rte_mbuf *udp_poll(uint64_t now, int *owner);
void udp_report_error(int fd, int error);
void udp_shutdown(void);

#endif
