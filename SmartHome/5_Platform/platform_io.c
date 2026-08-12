#include "platform_io.h"

#include "driver_led_key.h"
#include "driver_dbg.h"

void platform_io_init(struct IODev *dev)
{
	if(dev == NULL) return;
	switch(dev->Type)
	{
		case LED:
		{
			Driver_LED_Init();
			break;
		}
		case KEY:
		{
			Driver_Key_Init();
			break;
		}
		case DBGOUT:
		{
			Driver_DBG_Init();
			break;
		}
		default:break;
	}
}

int platform_io_write(struct IODev *dev, uint8_t *buf, uint16_t len)
{
	if(dev == NULL || buf == NULL || len == 0) return -1;

	switch(dev->Type)
	{
		case LED:
			return Driver_LED_WriteStatus(buf[0]);
		default:
			return -1;
	}
}

int platform_io_read(struct IODev *dev, uint8_t *buf, uint16_t len)
{
	if(dev == NULL || buf == NULL || len == 0) return -1;

	switch(dev->Type)
	{
		case KEY:
			return Driver_Key_Read(buf, len);
		default:
			return -1;
	}
}




