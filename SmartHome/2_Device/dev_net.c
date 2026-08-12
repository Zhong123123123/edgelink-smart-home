#include "dev_net.h"
#include <platform_net.h>
static int NetDev_Init(struct NetDev *net)
{
	return platform_net_init(net);
}
static int NetDev_Connect(struct NetDev *net, const char *arg, int timeout)
{
	return platform_net_connect(net, arg, timeout);
}
static int NetDev_Disconnect(struct NetDev *net, const char *arg, int timeout)
{
	return platform_net_disconnect(net, arg, timeout);
}
static int NetDev_Write(struct NetDev *net, char *buf, unsigned short len, int timeout)
{
	return platform_net_write(net, buf, len, timeout);
}
static int NetDev_Read(struct NetDev *net, char *buf, unsigned short len, int timeout)
{
	return platform_net_read(net, buf, len, timeout);
}

static NetDev g_tNetDev = {ESP8266, NetDev_Init, NetDev_Connect, NetDev_Disconnect, NetDev_Write, NetDev_Read};

ptNetDev NetDev_GetDev(NetDevType type)
{
	if(ESP8266 == type)
		return &g_tNetDev;
	
	return NULL;
}
