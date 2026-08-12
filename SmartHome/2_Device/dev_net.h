#ifndef __DEV_NET_H
#define __DEV_NET_H

typedef enum{
	ESP8266 = (1<<0),
	OTHERS = 0xFFFF,
}NetDevType;

typedef enum{
	WiFi_ID = (1<<0),
	UDP_ID  = (1<<1),
	TCP_ID  = (1<<2)
}ConnectID;

typedef struct{
	ConnectID id;
	char *ssid;
	char *pwd;
}WiFiInfo;

typedef struct{
	ConnectID id;
	char *IP;
	unsigned short LocalPort;
	unsigned short RemotePort;
}TCP_UDP_Info;

typedef struct NetDev{
	unsigned char Type;
	int (*Init)(struct NetDev *net);
	int (*Connect)(struct NetDev *net, const char *arg, int timeout);
	int (*Disconnect)(struct NetDev *net, const char *arg, int timeout);
	int (*Write)(struct NetDev *net, char *buf, unsigned short len, int timeout);
	int (*Read)(struct NetDev *net, char *buf, unsigned short len, int timeout);
}NetDev, *ptNetDev;

ptNetDev NetDev_GetDev(NetDevType type);

#endif
