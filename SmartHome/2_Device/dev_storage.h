#ifndef __DEV_STORAGE_H
#define __DEV_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

bool DevStorage_Init(void);
uint32_t DevStorage_ReadJEDECID(void);
uint32_t DevStorage_GetSizeBytes(void);
bool DevStorage_Read(uint32_t addr, uint8_t *buf, uint32_t len);
bool DevStorage_PageProgram(uint32_t addr, const uint8_t *buf, uint32_t len);
bool DevStorage_SectorErase(uint32_t addr);

#endif
