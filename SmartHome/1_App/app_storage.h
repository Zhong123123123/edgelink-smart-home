#ifndef __APP_STORAGE_H
#define __APP_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

typedef struct
{
	uint32_t event_valid_count;
	uint32_t last_seq;
	uint32_t event_write_addr;
	uint32_t cache_backlog;
	uint32_t flash_size;
} StorageStats;

typedef struct
{
	uint32_t seq;
	uint16_t len;
	uint8_t payload[192];
} StorageCachedRecord;

void vStartStorageTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority);
bool Storage_WriteEvent(const char *event);
bool Storage_WriteFault(const char *fault);
bool Storage_WriteCache(const uint8_t *payload, uint16_t len);
bool Storage_PeekCache(StorageCachedRecord *out);
bool Storage_MarkCacheAck(uint32_t seq);
uint32_t Storage_GetCacheBacklog(void);
bool Storage_GetStats(StorageStats *stats);

#endif
