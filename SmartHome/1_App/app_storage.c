#include "app_storage.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "dev_storage.h"
#include "log.h"
#include "app_watchdog.h"

#define STORAGE_FLASH_SIZE       (16UL * 1024UL * 1024UL)
#define STORAGE_META_BASE        0x000000UL
#define STORAGE_META_SIZE        0x010000UL
#define STORAGE_EVENT_BASE       0x010000UL
#define STORAGE_EVENT_SIZE       0x7F0000UL
#define STORAGE_CACHE_BASE       0x800000UL
#define STORAGE_CACHE_SIZE       0x800000UL

#define STORAGE_MAGIC            0x53484C47UL
#define STORAGE_VERSION          1U
#define STORAGE_MAX_PAYLOAD_LEN  192U
#define STORAGE_TASK_QUEUE_LEN   16U

#define STORAGE_REC_EVENT        1U
#define STORAGE_REC_CACHE        2U
#define STORAGE_REC_FAULT        3U

#define STORAGE_SECTOR_SIZE      4096UL
#define STORAGE_PAGE_SIZE        256UL

typedef struct
{
	uint32_t magic;
	uint16_t version;
	uint16_t type;
	uint32_t seq;
	uint32_t tick_ms;
	uint16_t payload_len;
	uint16_t flags;
	uint32_t crc32;
} StorageRecordHeader;

#define STORAGE_RECORD_MAX_SIZE  ((((uint32_t)sizeof(StorageRecordHeader) + STORAGE_MAX_PAYLOAD_LEN) + 3UL) & ~3UL)

typedef enum
{
	STORAGE_MSG_WRITE_EVENT = 0,
	STORAGE_MSG_WRITE_FAULT = 1,
	STORAGE_MSG_WRITE_CACHE = 2
} StorageMsgType;

typedef struct
{
	StorageMsgType type;
	uint16_t len;
	char payload[STORAGE_MAX_PAYLOAD_LEN];
} StorageMsg;

typedef struct
{
	uint8_t inited;
	uint32_t flash_size;
	uint32_t event_write_addr;
	uint32_t event_valid_count;
	uint32_t cache_read_addr;
	uint32_t cache_write_addr;
	uint32_t cache_backlog;
	uint32_t last_seq;
} StorageCtx;

static StorageCtx g_storage_ctx;
static QueueHandle_t g_storage_queue = NULL;

static uint32_t storage_align4(uint32_t value)
{
	return (value + 3UL) & ~3UL;
}

static uint32_t storage_crc32(const uint8_t *data, uint32_t len)
{
	uint32_t crc = 0xFFFFFFFFUL;
	uint32_t i;
	uint8_t bit;
	for (i = 0U; i < len; ++i)
	{
		crc ^= (uint32_t)data[i];
		for (bit = 0U; bit < 8U; ++bit)
		{
			crc = ((crc & 1UL) != 0UL) ? ((crc >> 1U) ^ 0xEDB88320UL) : (crc >> 1U);
		}
	}
	return ~crc;
}

static bool storage_flash_in_range(uint32_t addr, uint32_t len)
{
	if (len == 0U)
	{
		return true;
	}
	if (addr < STORAGE_EVENT_BASE || addr >= (STORAGE_EVENT_BASE + STORAGE_EVENT_SIZE))
	{
		return false;
	}
	if (len > ((STORAGE_EVENT_BASE + STORAGE_EVENT_SIZE) - addr))
	{
		return false;
	}
	return true;
}

static bool storage_cache_in_range(uint32_t addr, uint32_t len)
{
	if (len == 0U)
	{
		return true;
	}
	if (addr < STORAGE_CACHE_BASE || addr >= (STORAGE_CACHE_BASE + STORAGE_CACHE_SIZE))
	{
		return false;
	}
	if (len > ((STORAGE_CACHE_BASE + STORAGE_CACHE_SIZE) - addr))
	{
		return false;
	}
	return true;
}

static bool storage_flash_erase_range(uint32_t start_addr, uint32_t len)
{
	uint32_t addr;
	uint32_t end_addr;
	if (!storage_flash_in_range(start_addr, len))
	{
		return false;
	}
	addr = start_addr & ~(STORAGE_SECTOR_SIZE - 1UL);
	end_addr = storage_align4(start_addr + len);
	while (addr < end_addr)
	{
		if (!DevStorage_SectorErase(addr))
		{
			return false;
		}
		addr += STORAGE_SECTOR_SIZE;
	}
	return true;
}

static bool storage_flash_write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
	uint32_t written = 0U;
	if (buf == NULL)
	{
		return false;
	}
	if (!storage_flash_in_range(addr, len))
	{
		return false;
	}
	while (written < len)
	{
		uint32_t cur = addr + written;
		uint32_t page_remain = STORAGE_PAGE_SIZE - (cur & (STORAGE_PAGE_SIZE - 1UL));
		uint32_t chunk = len - written;
		if (chunk > page_remain)
		{
			chunk = page_remain;
		}
		if (!DevStorage_PageProgram(cur, &buf[written], chunk))
		{
			return false;
		}
		written += chunk;
	}
	return true;
}

static bool storage_flash_is_erased(uint32_t addr, uint32_t len)
{
	uint8_t tmp[64];
	uint32_t off = 0U;
	while (off < len)
	{
		uint32_t chunk = len - off;
		uint32_t i;
		if (chunk > sizeof(tmp))
		{
			chunk = sizeof(tmp);
		}
		if (!DevStorage_Read(addr + off, tmp, chunk))
		{
			return false;
		}
		for (i = 0U; i < chunk; ++i)
		{
			if (tmp[i] != 0xFFU)
			{
				return false;
			}
		}
		off += chunk;
	}
	return true;
}

static bool storage_cache_erase_range(uint32_t start_addr, uint32_t len)
{
	uint32_t addr;
	uint32_t end_addr;
	if (!storage_cache_in_range(start_addr, len))
	{
		return false;
	}
	addr = start_addr & ~(STORAGE_SECTOR_SIZE - 1UL);
	end_addr = storage_align4(start_addr + len);
	while (addr < end_addr)
	{
		if (!DevStorage_SectorErase(addr))
		{
			return false;
		}
		addr += STORAGE_SECTOR_SIZE;
	}
	return true;
}

static bool storage_cache_write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
	uint32_t written = 0U;
	if (buf == NULL || !storage_cache_in_range(addr, len))
	{
		return false;
	}
	while (written < len)
	{
		uint32_t cur = addr + written;
		uint32_t page_remain = STORAGE_PAGE_SIZE - (cur & (STORAGE_PAGE_SIZE - 1UL));
		uint32_t chunk = len - written;
		if (chunk > page_remain)
		{
			chunk = page_remain;
		}
		if (!DevStorage_PageProgram(cur, &buf[written], chunk))
		{
			return false;
		}
		written += chunk;
	}
	return true;
}

static bool storage_cache_is_erased(uint32_t addr, uint32_t len)
{
	uint8_t tmp[64];
	uint32_t off = 0U;
	if (!storage_cache_in_range(addr, len))
	{
		return false;
	}
	while (off < len)
	{
		uint32_t chunk = len - off;
		uint32_t i;
		if (chunk > sizeof(tmp))
		{
			chunk = sizeof(tmp);
		}
		if (!DevStorage_Read(addr + off, tmp, chunk))
		{
			return false;
		}
		for (i = 0U; i < chunk; ++i)
		{
			if (tmp[i] != 0xFFU)
			{
				return false;
			}
		}
		off += chunk;
	}
	return true;
}

static bool storage_scan_event_region(void)
{
	StorageRecordHeader hdr;
	uint8_t payload[STORAGE_MAX_PAYLOAD_LEN + 4U];
	uint32_t addr = STORAGE_EVENT_BASE;
	uint32_t end = STORAGE_EVENT_BASE + STORAGE_EVENT_SIZE;
	g_storage_ctx.event_valid_count = 0U;
	g_storage_ctx.last_seq = 0U;

	while ((addr + sizeof(StorageRecordHeader)) <= end)
	{
		uint32_t rec_len;
		uint32_t stored_crc;
		uint32_t calc_crc;
		uint32_t crc_off = (uint32_t)offsetof(StorageRecordHeader, crc32);

		if (!DevStorage_Read(addr, (uint8_t *)&hdr, sizeof(hdr)))
		{
			return false;
		}
		if (hdr.magic == 0xFFFFFFFFUL)
		{
			break;
		}
		if (hdr.magic != STORAGE_MAGIC || hdr.version != STORAGE_VERSION)
		{
			LOG_WARN("STORAGE", "scan stop invalid header addr=0x%08lX", (unsigned long)addr);
			break;
		}
		if (hdr.payload_len > STORAGE_MAX_PAYLOAD_LEN)
		{
			LOG_WARN("STORAGE", "scan stop invalid len=%u addr=0x%08lX",
				(unsigned int)hdr.payload_len,
				(unsigned long)addr);
			break;
		}
		rec_len = storage_align4((uint32_t)sizeof(StorageRecordHeader) + (uint32_t)hdr.payload_len);
		if ((addr + rec_len) > end)
		{
			LOG_WARN("STORAGE", "scan stop overflow addr=0x%08lX", (unsigned long)addr);
			break;
		}
		if (!DevStorage_Read(addr + sizeof(StorageRecordHeader), payload, rec_len - (uint32_t)sizeof(StorageRecordHeader)))
		{
			return false;
		}

		stored_crc = hdr.crc32;
		hdr.crc32 = 0U;
		calc_crc = storage_crc32((const uint8_t *)&hdr, crc_off + 4U);
		calc_crc = storage_crc32(payload, (uint32_t)hdr.payload_len) ^ (calc_crc << 1U);
		if (stored_crc != calc_crc)
		{
			LOG_WARN("STORAGE", "scan stop crc mismatch seq=%lu addr=0x%08lX",
				(unsigned long)hdr.seq,
				(unsigned long)addr);
			break;
		}

		g_storage_ctx.event_valid_count++;
		if (hdr.seq > g_storage_ctx.last_seq)
		{
			g_storage_ctx.last_seq = hdr.seq;
		}
		addr += rec_len;
	}

	g_storage_ctx.event_write_addr = addr;
	LOG_INFO("STORAGE", "scan event valid=%lu last_seq=%lu",
		(unsigned long)g_storage_ctx.event_valid_count,
		(unsigned long)g_storage_ctx.last_seq);
	return true;
}

static bool storage_scan_cache_region(void)
{
	StorageRecordHeader hdr;
	uint8_t payload[STORAGE_MAX_PAYLOAD_LEN + 4U];
	uint32_t addr = STORAGE_CACHE_BASE;
	uint32_t end = STORAGE_CACHE_BASE + STORAGE_CACHE_SIZE;
	g_storage_ctx.cache_backlog = 0U;
	g_storage_ctx.cache_read_addr = STORAGE_CACHE_BASE;

	while ((addr + sizeof(StorageRecordHeader)) <= end)
	{
		uint32_t rec_len;
		uint32_t stored_crc;
		uint32_t calc_crc;
		uint32_t crc_off = (uint32_t)offsetof(StorageRecordHeader, crc32);

		if (!DevStorage_Read(addr, (uint8_t *)&hdr, sizeof(hdr)))
		{
			return false;
		}
		if (hdr.magic == 0xFFFFFFFFUL)
		{
			break;
		}
		if (hdr.magic != STORAGE_MAGIC || hdr.version != STORAGE_VERSION || hdr.type != STORAGE_REC_CACHE)
		{
			break;
		}
		if (hdr.payload_len == 0U || hdr.payload_len > STORAGE_MAX_PAYLOAD_LEN)
		{
			break;
		}
		rec_len = storage_align4((uint32_t)sizeof(StorageRecordHeader) + (uint32_t)hdr.payload_len);
		if ((addr + rec_len) > end)
		{
			break;
		}
		if (!DevStorage_Read(addr + sizeof(StorageRecordHeader), payload, rec_len - (uint32_t)sizeof(StorageRecordHeader)))
		{
			return false;
		}
		stored_crc = hdr.crc32;
		hdr.crc32 = 0U;
		calc_crc = storage_crc32((const uint8_t *)&hdr, crc_off + 4U);
		calc_crc = storage_crc32(payload, (uint32_t)hdr.payload_len) ^ (calc_crc << 1U);
		if (stored_crc != calc_crc)
		{
			break;
		}
		g_storage_ctx.cache_backlog++;
		if (hdr.seq > g_storage_ctx.last_seq)
		{
			g_storage_ctx.last_seq = hdr.seq;
		}
		addr += rec_len;
	}

	g_storage_ctx.cache_write_addr = addr;
	LOG_INFO("STORAGE", "scan cache backlog=%lu", (unsigned long)g_storage_ctx.cache_backlog);
	return true;
}

static bool storage_write_record(uint16_t rec_type, const uint8_t *payload, uint16_t payload_len)
{
	StorageRecordHeader hdr;
	uint8_t buf[STORAGE_RECORD_MAX_SIZE];
	uint8_t verify[STORAGE_RECORD_MAX_SIZE];
	uint32_t total_len;
	uint32_t end = STORAGE_EVENT_BASE + STORAGE_EVENT_SIZE;
	uint32_t write_addr;
	uint32_t crc_off = (uint32_t)offsetof(StorageRecordHeader, crc32);

	if (payload == NULL || payload_len == 0U || payload_len > STORAGE_MAX_PAYLOAD_LEN)
	{
		return false;
	}

	total_len = storage_align4((uint32_t)sizeof(StorageRecordHeader) + (uint32_t)payload_len);
	if ((g_storage_ctx.event_write_addr + total_len) > end)
	{
		g_storage_ctx.event_write_addr = STORAGE_EVENT_BASE;
	}
	write_addr = g_storage_ctx.event_write_addr;
	if (!storage_flash_is_erased(write_addr, total_len))
	{
		if (!storage_flash_erase_range(write_addr, total_len))
		{
			return false;
		}
	}

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = STORAGE_MAGIC;
	hdr.version = STORAGE_VERSION;
	hdr.type = rec_type;
	hdr.seq = g_storage_ctx.last_seq + 1U;
	hdr.tick_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
	hdr.payload_len = payload_len;
	hdr.flags = 0U;
	hdr.crc32 = 0U;

	memset(buf, 0xFF, total_len);
	memcpy(buf, &hdr, sizeof(hdr));
	memcpy(&buf[sizeof(hdr)], payload, payload_len);

	hdr.crc32 = storage_crc32((const uint8_t *)&hdr, crc_off + 4U);
	hdr.crc32 = storage_crc32(payload, payload_len) ^ (hdr.crc32 << 1U);
	memcpy(&buf[crc_off], &hdr.crc32, sizeof(hdr.crc32));

	if (!storage_flash_write(write_addr, buf, total_len))
	{
		return false;
	}
	if (!DevStorage_Read(write_addr, verify, total_len))
	{
		LOG_ERROR("STORAGE", "verify read failed addr=0x%08lX len=%lu",
			(unsigned long)write_addr,
			(unsigned long)total_len);
		return false;
	}
	if (memcmp(buf, verify, total_len) != 0)
	{
		LOG_ERROR("STORAGE", "verify failed addr=0x%08lX len=%lu",
			(unsigned long)write_addr,
			(unsigned long)total_len);
		return false;
	}

	g_storage_ctx.last_seq = hdr.seq;
	g_storage_ctx.event_valid_count++;
	g_storage_ctx.event_write_addr = write_addr + total_len;

	if (rec_type == STORAGE_REC_FAULT)
	{
		LOG_INFO("STORAGE", "fault write seq=%lu len=%u", (unsigned long)hdr.seq, (unsigned int)payload_len);
	}
	else if (rec_type == STORAGE_REC_EVENT)
	{
		char text[80];
		uint16_t n = payload_len;
		if (n >= sizeof(text))
		{
			n = (uint16_t)(sizeof(text) - 1U);
		}
		memcpy(text, payload, n);
		text[n] = '\0';
		LOG_INFO("STORAGE", "event write seq=%lu event=%s", (unsigned long)hdr.seq, text);
	}
	else
	{
		LOG_INFO("STORAGE", "event write seq=%lu len=%u", (unsigned long)hdr.seq, (unsigned int)payload_len);
	}
	return true;
}

static bool storage_write_cache_record(const uint8_t *payload, uint16_t payload_len)
{
	StorageRecordHeader hdr;
	uint8_t buf[STORAGE_RECORD_MAX_SIZE];
	uint8_t verify[STORAGE_RECORD_MAX_SIZE];
	uint32_t total_len;
	uint32_t write_addr;
	uint32_t end = STORAGE_CACHE_BASE + STORAGE_CACHE_SIZE;
	uint32_t crc_off = (uint32_t)offsetof(StorageRecordHeader, crc32);

	if (payload == NULL || payload_len == 0U || payload_len > STORAGE_MAX_PAYLOAD_LEN)
	{
		return false;
	}
	total_len = storage_align4((uint32_t)sizeof(StorageRecordHeader) + (uint32_t)payload_len);
	if ((g_storage_ctx.cache_write_addr + total_len) > end)
	{
		g_storage_ctx.cache_write_addr = STORAGE_CACHE_BASE;
		g_storage_ctx.cache_read_addr = STORAGE_CACHE_BASE;
		g_storage_ctx.cache_backlog = 0U;
	}
	write_addr = g_storage_ctx.cache_write_addr;
	if (!storage_cache_is_erased(write_addr, total_len))
	{
		if (!storage_cache_erase_range(write_addr, total_len))
		{
			return false;
		}
	}

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = STORAGE_MAGIC;
	hdr.version = STORAGE_VERSION;
	hdr.type = STORAGE_REC_CACHE;
	hdr.seq = g_storage_ctx.last_seq + 1U;
	hdr.tick_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
	hdr.payload_len = payload_len;
	hdr.flags = 0U;
	hdr.crc32 = 0U;

	memset(buf, 0xFF, total_len);
	memcpy(buf, &hdr, sizeof(hdr));
	memcpy(&buf[sizeof(hdr)], payload, payload_len);
	hdr.crc32 = storage_crc32((const uint8_t *)&hdr, crc_off + 4U);
	hdr.crc32 = storage_crc32(payload, payload_len) ^ (hdr.crc32 << 1U);
	memcpy(&buf[crc_off], &hdr.crc32, sizeof(hdr.crc32));

	if (!storage_cache_write(write_addr, buf, total_len))
	{
		return false;
	}
	if (!DevStorage_Read(write_addr, verify, total_len) || memcmp(buf, verify, total_len) != 0)
	{
		LOG_ERROR("STORAGE", "cache verify failed addr=0x%08lX", (unsigned long)write_addr);
		return false;
	}
	g_storage_ctx.last_seq = hdr.seq;
	g_storage_ctx.cache_write_addr = write_addr + total_len;
	g_storage_ctx.cache_backlog++;
	LOG_INFO("STORAGE", "cache write seq=%lu len=%u backlog=%lu",
		(unsigned long)hdr.seq,
		(unsigned int)payload_len,
		(unsigned long)g_storage_ctx.cache_backlog);
	return true;
}

static void StorageTask(void *pvParameters)
{
	StorageMsg msg;
	uint32_t jedec;
	uint32_t size_kb;
	(void)pvParameters;

	memset(&g_storage_ctx, 0, sizeof(g_storage_ctx));
	if (!DevStorage_Init())
	{
		jedec = DevStorage_ReadJEDECID();
		LOG_ERROR("STORAGE", "flash init failed jedec=0x%08lX", (unsigned long)jedec);
		vTaskDelete(NULL);
		return;
	}

	jedec = DevStorage_ReadJEDECID();
	g_storage_ctx.flash_size = DevStorage_GetSizeBytes();
	size_kb = g_storage_ctx.flash_size / 1024UL;
	LOG_INFO("STORAGE", "flash init ok jedec=0x%08lX size=%luKB",
		(unsigned long)jedec,
		(unsigned long)size_kb);
	if (g_storage_ctx.flash_size != STORAGE_FLASH_SIZE)
	{
		LOG_WARN("STORAGE", "flash size mismatch actual=%luKB expected=%luKB",
			(unsigned long)size_kb,
			(unsigned long)(STORAGE_FLASH_SIZE / 1024UL));
	}

	if (!storage_scan_event_region())
	{
		LOG_ERROR("STORAGE", "scan failed");
	}
	if (!storage_scan_cache_region())
	{
		LOG_ERROR("STORAGE", "scan cache failed");
	}
	g_storage_ctx.inited = 1U;
	(void)storage_write_record(STORAGE_REC_EVENT, (const uint8_t *)"event=boot", (uint16_t)strlen("event=boot"));

	while (1)
	{
		Watchdog_Kick(WD_TASK_STORAGE);
		if (xQueueReceive(g_storage_queue, &msg, pdMS_TO_TICKS(200U)) != pdTRUE)
		{
			continue;
		}
		if (msg.type == STORAGE_MSG_WRITE_EVENT)
		{
			(void)storage_write_record(STORAGE_REC_EVENT, (const uint8_t *)msg.payload, msg.len);
		}
		else if (msg.type == STORAGE_MSG_WRITE_FAULT)
		{
			(void)storage_write_record(STORAGE_REC_FAULT, (const uint8_t *)msg.payload, msg.len);
		}
		else if (msg.type == STORAGE_MSG_WRITE_CACHE)
		{
			(void)storage_write_cache_record((const uint8_t *)msg.payload, msg.len);
		}
	}
}

static bool storage_queue_text(StorageMsgType type, const char *text)
{
	StorageMsg msg;
	size_t len;
	if (text == NULL || g_storage_queue == NULL)
	{
		return false;
	}
	len = strlen(text);
	if (len == 0U)
	{
		return false;
	}
	if (len > STORAGE_MAX_PAYLOAD_LEN)
	{
		len = STORAGE_MAX_PAYLOAD_LEN;
	}
	memset(&msg, 0, sizeof(msg));
	msg.type = type;
	msg.len = (uint16_t)len;
	memcpy(msg.payload, text, len);
	if (xQueueSend(g_storage_queue, &msg, 0U) != pdTRUE)
	{
		LOG_WARN("STORAGE", "queue full drop type=%u", (unsigned int)type);
		return false;
	}
	return true;
}

bool Storage_WriteEvent(const char *event)
{
	return storage_queue_text(STORAGE_MSG_WRITE_EVENT, event);
}

bool Storage_WriteFault(const char *fault)
{
	return storage_queue_text(STORAGE_MSG_WRITE_FAULT, fault);
}

bool Storage_WriteCache(const uint8_t *payload, uint16_t len)
{
	StorageMsg msg;
	if (payload == NULL || len == 0U || len > STORAGE_MAX_PAYLOAD_LEN || g_storage_queue == NULL)
	{
		return false;
	}
	memset(&msg, 0, sizeof(msg));
	msg.type = STORAGE_MSG_WRITE_CACHE;
	msg.len = len;
	memcpy(msg.payload, payload, len);
	if (xQueueSend(g_storage_queue, &msg, 0U) != pdTRUE)
	{
		LOG_WARN("STORAGE", "queue full drop cache len=%u", (unsigned int)len);
		return false;
	}
	return true;
}

bool Storage_PeekCache(StorageCachedRecord *out)
{
	StorageRecordHeader hdr;
	uint32_t rec_len;
	if (out == NULL || g_storage_ctx.cache_backlog == 0U)
	{
		if (g_storage_ctx.cache_backlog != 0U)
		{
			LOG_WARN("STORAGE", "cache peek invalid out backlog=%lu", (unsigned long)g_storage_ctx.cache_backlog);
		}
		return false;
	}
	if (!DevStorage_Read(g_storage_ctx.cache_read_addr, (uint8_t *)&hdr, sizeof(hdr)))
	{
		LOG_WARN("STORAGE", "cache peek header read fail addr=0x%08lX backlog=%lu",
			(unsigned long)g_storage_ctx.cache_read_addr,
			(unsigned long)g_storage_ctx.cache_backlog);
		return false;
	}
	if (hdr.magic != STORAGE_MAGIC || hdr.type != STORAGE_REC_CACHE || hdr.payload_len == 0U || hdr.payload_len > STORAGE_MAX_PAYLOAD_LEN)
	{
		LOG_WARN("STORAGE", "cache peek invalid hdr addr=0x%08lX magic=0x%08lX type=%lu len=%u backlog=%lu",
			(unsigned long)g_storage_ctx.cache_read_addr,
			(unsigned long)hdr.magic,
			(unsigned long)hdr.type,
			(unsigned int)hdr.payload_len,
			(unsigned long)g_storage_ctx.cache_backlog);
		(void)storage_scan_cache_region();
		return false;
	}
	rec_len = storage_align4((uint32_t)sizeof(StorageRecordHeader) + (uint32_t)hdr.payload_len);
	if (!storage_cache_in_range(g_storage_ctx.cache_read_addr, rec_len))
	{
		LOG_WARN("STORAGE", "cache peek out of range addr=0x%08lX rec_len=%lu backlog=%lu",
			(unsigned long)g_storage_ctx.cache_read_addr,
			(unsigned long)rec_len,
			(unsigned long)g_storage_ctx.cache_backlog);
		return false;
	}
	if (!DevStorage_Read(g_storage_ctx.cache_read_addr + sizeof(StorageRecordHeader), out->payload, hdr.payload_len))
	{
		LOG_WARN("STORAGE", "cache peek payload read fail addr=0x%08lX len=%u backlog=%lu",
			(unsigned long)(g_storage_ctx.cache_read_addr + sizeof(StorageRecordHeader)),
			(unsigned int)hdr.payload_len,
			(unsigned long)g_storage_ctx.cache_backlog);
		return false;
	}
	out->seq = hdr.seq;
	out->len = hdr.payload_len;
	LOG_INFO("STORAGE", "cache peek seq=%lu len=%u", (unsigned long)out->seq, (unsigned int)out->len);
	return true;
}

bool Storage_MarkCacheAck(uint32_t seq)
{
	StorageRecordHeader hdr;
	uint32_t rec_len;
	uint32_t end = STORAGE_CACHE_BASE + STORAGE_CACHE_SIZE;
	if (g_storage_ctx.cache_backlog == 0U)
	{
		return false;
	}
	if (!DevStorage_Read(g_storage_ctx.cache_read_addr, (uint8_t *)&hdr, sizeof(hdr)))
	{
		return false;
	}
	if (hdr.seq != seq)
	{
		return false;
	}
	rec_len = storage_align4((uint32_t)sizeof(StorageRecordHeader) + (uint32_t)hdr.payload_len);
	g_storage_ctx.cache_read_addr += rec_len;
	if (g_storage_ctx.cache_read_addr >= end)
	{
		g_storage_ctx.cache_read_addr = STORAGE_CACHE_BASE;
	}
	if (g_storage_ctx.cache_backlog > 0U)
	{
		g_storage_ctx.cache_backlog--;
	}
	LOG_INFO("STORAGE", "cache ack seq=%lu backlog=%lu", (unsigned long)seq, (unsigned long)g_storage_ctx.cache_backlog);
	return true;
}

uint32_t Storage_GetCacheBacklog(void)
{
	return g_storage_ctx.cache_backlog;
}

bool Storage_GetStats(StorageStats *stats)
{
	if (stats == NULL)
	{
		return false;
	}
	stats->event_valid_count = g_storage_ctx.event_valid_count;
	stats->last_seq = g_storage_ctx.last_seq;
	stats->event_write_addr = g_storage_ctx.event_write_addr;
	stats->cache_backlog = g_storage_ctx.cache_backlog;
	stats->flash_size = g_storage_ctx.flash_size;
	return (g_storage_ctx.inited != 0U);
}

void vStartStorageTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority)
{
	if (g_storage_queue == NULL)
	{
		g_storage_queue = xQueueCreate(STORAGE_TASK_QUEUE_LEN, sizeof(StorageMsg));
		if (g_storage_queue == NULL)
		{
			LOG_ERROR("STORAGE", "create queue failed");
			return;
		}
	}

	if (xTaskCreate(StorageTask, "Storage", usTaskStackSize, NULL, uxTaskPriority, NULL) == pdPASS)
	{
		LOG_INFO("STORAGE", "create task success");
	}
	else
	{
		LOG_ERROR("STORAGE", "create task failed");
	}
}



