#ifndef __DEIVER_NET_H
#define	__DEIVER_NET_H

#include "main.h"


int Driver_Net_Init(void);
int Driver_Net_TransmitSocket(const char *socket, int len, int timeout);
int Driver_Net_RecvSocket(char *buf, int len, int timeout);
int Driver_Net_ConnectWiFi(const char *ssid, const char *pwd, int timeout);
int Driver_Net_DisconnectWiFi(void);
int Driver_Net_ConnectTCP(const char *ip, int port, int timeout);
int Driver_Net_Disconnect_TCP_UDP(void);
uint32_t Driver_Net_GetRxIsrDropCount(void);
void Driver_Net_ClearRxIsrDropCount(void);







#endif
