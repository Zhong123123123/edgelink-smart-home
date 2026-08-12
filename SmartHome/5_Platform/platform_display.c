#include "platform_display.h"

#include "driver_display.h"

int platform_display_init(struct DisplayDev *dev)
{
	if (dev == 0)
	{
		return -1;
	}

	return Driver_Display_Init();
}

int platform_display_show(struct DisplayDev *dev, const DisplayStatus *status)
{
	if (dev == 0 || status == 0)
	{
		return -1;
	}

	return Driver_Display_Show(status);
}
