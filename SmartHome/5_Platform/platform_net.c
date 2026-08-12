#include "platform_net.h"
#include <driver_net.h>

int platform_net_init(struct NetDev *net)
{
	if(net == NULL)return -1;
	return Driver_Net_Init();
}
int platform_net_connect(struct NetDev *net, const char *arg, int timeout)
{
	int ret = -1;
	if(net == NULL || arg == NULL)return -1;
	switch(arg[0])
	{
		case WiFi_ID:
		{
			WiFiInfo *p = (WiFiInfo *)arg;
			ret = Driver_Net_ConnectWiFi(p->ssid, p->pwd, timeout);
			break;
		}
		case TCP_ID:
		{
			TCP_UDP_Info *p = (TCP_UDP_Info *)arg;
			ret = Driver_Net_ConnectTCP(p->IP, p->RemotePort, 500);
		}
		default:break;
	}
	return ret;
}
int platform_net_disconnect(struct NetDev *net, const char *arg, int timeout)
{
	int ret = -1;
	if(net == NULL || arg == NULL)return -1;
	switch(arg[0])
	{
		case WiFi_ID:
		{
			ret = Driver_Net_DisconnectWiFi();
			break;
		}
		case TCP_ID:
		{
			ret = Driver_Net_Disconnect_TCP_UDP();
		}
		default:break;
	}
	return ret;
}
int platform_net_write(struct NetDev *net, char *buf, uint16_t len, int timeout)
{
	if(net == NULL || buf == NULL || len == 0)return -1;
	return Driver_Net_TransmitSocket(buf, len, timeout);
}
int platform_net_read(struct NetDev *net, char *buf, uint16_t len, int timeout)
{
	if(net == NULL || buf == NULL || len == 0)return -1;
	return Driver_Net_RecvSocket(buf, len, timeout);
}




