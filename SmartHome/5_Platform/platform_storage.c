#include "platform_storage.h"

#include "driver_w25qxx.h"

bool platform_storage_init(void)
{
	return W25QXX_Init();
}

uint32_t platform_storage_read_jedec_id(void)
{
	return W25QXX_ReadJEDECID();
}

uint32_t platform_storage_get_size_bytes(void)
{
	return W25QXX_GetSizeBytes();
}

bool platform_storage_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
	return W25QXX_Read(addr, buf, len);
}

bool platform_storage_page_program(uint32_t addr, const uint8_t *buf, uint32_t len)
{
	return W25QXX_PageProgram(addr, buf, len);
}

bool platform_storage_sector_erase(uint32_t addr)
{
	return W25QXX_SectorErase(addr);
}
