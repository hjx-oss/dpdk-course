#ifndef IPV4_H
#define IPV4_H

#include <stdbool.h>
#include <rte_mbuf.h>

/* 检查 IPv4 后分发到 UDP 或 ICMP；调用者负责发送或释放。 */
bool ipv4_reply(struct rte_mbuf *packet);

#endif
