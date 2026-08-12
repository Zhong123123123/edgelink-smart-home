#include "dev_storage.h"

#include "platform_storage.h"

bool DevStorage_Init(void)
{
	return platform_storage_init();
}

uint32_t DevStorage_ReadJEDECID(void)
{
	return platform_storage_read_jedec_id();
}

uint32_t DevStorage_GetSizeBytes(void)
{
	return platform_storage_get_size_bytes();
}

bool DevStorage_Read(uint32_t addr, uint8_t *buf, uint32_t len)
{
	return platform_storage_read(addr, buf, len);
}

bool DevStorage_PageProgram(uint32_t addr, const uint8_t *buf, uint32_t len)
{
	return platform_storage_page_program(addr, buf, len);
}

bool DevStorage_SectorErase(uint32_t addr)
{
	return platform_storage_sector_erase(addr);
}
