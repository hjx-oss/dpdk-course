#ifndef UDP_H
#define UDP_H

#include <stdbool.h>
#include <rte_mbuf.h>

/* 原地把合法请求改成回复。无论返回什么，mbuf 的所有权仍在调用者。 */
bool udp_reply(struct rte_mbuf *packet);

#endif
