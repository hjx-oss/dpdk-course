#ifndef IPV4_H
#define IPV4_H

#include <stdbool.h>
#include <rte_mbuf.h>

enum ipv4_result {
    IPV4_DROP,
    IPV4_CONSUMED,
    IPV4_REPLY_READY
};

/* UDP 已复制进应用队列；ICMP 可能在原 mbuf 中准备好回复。 */
enum ipv4_result ipv4_input(struct rte_mbuf *packet);

#endif
