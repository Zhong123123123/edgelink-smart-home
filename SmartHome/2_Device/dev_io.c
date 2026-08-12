#include "dev_io.h"
#include <platform_io.h>
static void IODev_Init(struct IODev *dev)
{
	platform_io_init(dev);
}

static int IODev_Write(struct IODev *dev, uint8_t *buf, uint16_t len)
{
	return platform_io_write(dev, buf, len);
}

static int IODev_Read(struct IODev *dev, uint8_t *buf, uint16_t len)
{
	return platform_io_read(dev, buf, len);
}

static const IODev g_tIODevs[3] = {{LED, IODev_Init, IODev_Write, IODev_Read},\
								{KEY, IODev_Init, IODev_Write, IODev_Read},\
								{DBGOUT, IODev_Init, IODev_Write, IODev_Read}};

ptIODev IODev_GetDev(IODevType type)
{
	return (ptIODev)&g_tIODevs[type];
}









