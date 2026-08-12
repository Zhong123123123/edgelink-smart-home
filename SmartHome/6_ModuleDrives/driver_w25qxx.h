#ifndef __DRIVER_W25QXX_H
#define __DRIVER_W25QXX_H

#include <stdbool.h>
#include <stdint.h>

bool W25QXX_Init(void);
uint32_t W25QXX_ReadJEDECID(void);
bool W25QXX_Read(uint32_t addr, uint8_t *buf, uint32_t len);
bool W25QXX_PageProgram(uint32_t addr, const uint8_t *buf, uint32_t len);
bool W25QXX_SectorErase(uint32_t addr);
bool W25QXX_WaitBusy(uint32_t timeout_ms);
uint32_t W25QXX_GetSizeBytes(void);

#endif
