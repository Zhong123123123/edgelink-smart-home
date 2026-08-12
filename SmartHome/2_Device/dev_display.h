#ifndef __DEV_DISPLAY_H
#define __DEV_DISPLAY_H

#include <stdint.h>

#include "driver_display.h"

typedef enum
{
	DISPLAY_UART_SIM = (1 << 0),
	DISPLAY_OTHERS = 0xFFFF
} DisplayDevType;

typedef struct DisplayDev
{
	uint16_t type;
	int (*Init)(struct DisplayDev *dev);
	int (*Show)(struct DisplayDev *dev, const DisplayStatus *status);
} DisplayDev, *ptDisplayDev;

ptDisplayDev DisplayDev_GetDev(DisplayDevType type);

#endif
