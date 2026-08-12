#ifndef __PLATFORM_NET_H
#define __PLATFORM_NET_H

#include "platform.h"
#include "dev_net.h"

int platform_net_init(struct NetDev *net);
int platform_net_connect(struct NetDev *net, const char *arg, int timeout);
int platform_net_disconnect(struct NetDev *net, const char *arg, int timeout);
int platform_net_write(struct NetDev *net, char *buf, uint16_t len, int timeout);
int platform_net_read(struct NetDev *net, char *buf, uint16_t len, int timeout);

#endif
