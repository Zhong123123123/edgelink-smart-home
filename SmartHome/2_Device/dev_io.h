#ifndef __DEV_IO_H
#define __DEV_IO_H

#include "platform.h"

typedef enum{
	LED = (0),
	KEY = (1),
	DBGOUT = (2),
}IODevType;

typedef struct{
	uint16_t num;
	uint16_t time;
}KeyEvent;

typedef struct IODev{
	IODevType Type;
	void (*Init)(struct IODev *dev);
	int (*write)(struct IODev *dev, uint8_t *buf, uint16_t len);
	int (*Read)(struct IODev *dev, uint8_t *buf, uint16_t len);
}IODev, *ptIODev;

ptIODev IODev_GetDev(IODevType type);

#endif
