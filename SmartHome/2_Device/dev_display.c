#include "dev_display.h"

#include "platform_display.h"

static int DisplayDev_Init(struct DisplayDev *dev)
{
	return platform_display_init(dev);
}

static int DisplayDev_Show(struct DisplayDev *dev, const DisplayStatus *status)
{
	return platform_display_show(dev, status);
}

static const DisplayDev g_display_dev = {DISPLAY_UART_SIM, DisplayDev_Init, DisplayDev_Show};

ptDisplayDev DisplayDev_GetDev(DisplayDevType type)
{
	if (type == DISPLAY_UART_SIM)
	{
		return (ptDisplayDev)&g_display_dev;
	}

	return 0;
}
