#ifndef ARP_H
#define ARP_H

#include <stdbool.h>
#include <rte_ether.h>
#include <rte_mbuf.h>

/* 本课由一个收发线程调用这些接口。 */
enum arp_state {
    ARP_EMPTY,
    ARP_PENDING,
    ARP_REACHABLE,
    ARP_FAILED
};

enum arp_result {
    ARP_IGNORED,
    ARP_LEARNED,
    ARP_REPLY_READY
};

void arp_init(const struct rte_ether_addr *local_mac);

/* 非阻塞查询：仅返回 REACHABLE 时写出 mac；now 使用计时器刻度。 */
enum arp_state arp_resolve(rte_be32_t ip, struct rte_ether_addr *mac, uint64_t now);

/* 返回到期的新请求；调用者负责提交 TX，或释放未被 TX 接收的 mbuf。 */
struct rte_mbuf *arp_poll(struct rte_mempool *pool, uint64_t now);

/* 只改内容和邻居表，不发送、不释放接收到的 mbuf。 */
enum arp_result arp_input(struct rte_mbuf *packet, uint64_t now);

#endif
