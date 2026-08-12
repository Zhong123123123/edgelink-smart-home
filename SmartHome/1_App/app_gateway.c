/* Serial Gateway Mode (A plan). */
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "app_gateway.h"
#include "board_pins.h"
#include "config.h"
#include "log.h"
#include "main.h"
#include "smarthome_state.h"
#include "app_watchdog.h"
#include "app_storage.h"
#include "fault_diag.h"
#include "platform_backup.h"
#include "driver_led_key.h"

extern TaskHandle_t ledTaskHandle;
extern uint32_t __Vectors;

static volatile uint8_t g_gateway_force_report = 0U;
static volatile uint8_t g_app_confirm_sent = 0U;
static QueueHandle_t g_gateway_event_queue = NULL;

void Gateway_RequestImmediateReport(void)
{
	g_gateway_force_report = 1U;
}

void Gateway_ReportAlarmEvent(uint8_t event_code)
{
	if (g_gateway_event_queue == NULL)
	{
		return;
	}
	if (xQueueSend(g_gateway_event_queue, &event_code, 0U) != pdTRUE)
	{
		LOG_WARN("GATEWAY", "event queue full drop code=%u", (unsigned int)event_code);
	}
}


#define GW_UART_BAUDRATE              BOARD_UART_BAUD_DEFAULT
#define GW_DEVICE_ID                  1U
#define GW_HEARTBEAT_PERIOD_MS        10000U
#define GW_VOLTAGE_MV_DEFAULT         3300U

#define GW_FRAME_HEAD_0               0xAAU
#define GW_FRAME_HEAD_1               0x55U

#define GW_TYPE_SENSOR_DATA           0x01U
#define GW_TYPE_ALARM_EVENT           0x02U
#define GW_TYPE_HEARTBEAT             0x03U
#define GW_TYPE_COMMAND_ACK           0x04U
#define GW_TYPE_DEVICE_STATUS         0x05U
#define GW_TYPE_REPORT_ACK            0x06U
#define GW_TYPE_COMMAND_REQ           0x10U
#define GW_TYPE_APP_CONFIRM           0x26U

#define GW_CMD_SET_LED                0x01U
#define GW_CMD_SET_BUZZER             0x02U
#define GW_CMD_SET_MODE               0x03U
#define GW_CMD_GET_STATUS             0x04U
#define GW_CMD_SET_THRESHOLD          0x05U
#define GW_CMD_SET_LOG_LEVEL          0x06U
#define GW_CMD_REBOOT_TO_BOOTLOADER   0x07U

#define GW_ACK_OK                     0x00U
#define GW_ACK_BAD_PAYLOAD            0x01U
#define GW_ACK_UNKNOWN_CMD            0x02U

#define GW_RX_RING_SIZE               512U
#define GW_FRAME_BUF_SIZE             128U
#define GW_TX_FRAME_MAX_SIZE          96U
#define GW_DIAG_HEARTBEAT_MS          30000U
#define GW_APP_CONFIRM_DELAY_MS       30000U
#define GW_APP_VERSION_MAJOR          1U
#define GW_APP_VERSION_MINOR          0U
#define GW_APP_VERSION_PATCH          0U
#define GW_REPLAY_BATCH_MAX           5U
#define GW_REPLAY_RETRY_INTERVAL_MS   1000U
#define GW_REPORT_ACK_TIMEOUT_MS      1200U
#define GW_PENDING_QUEUE_SIZE         8U
#define GW_TX_HEX_DUMP_MAX_FRAMES     8U
#define GW_EVENT_QUEUE_LEN            8U

#if defined(SH_MCU_F407)
#define GW_UART_RX_DMA_INSTANCE       DMA1_Stream1
#define GW_UART_RX_DMA_CHANNEL        DMA_CHANNEL_4
#define GW_UART_RX_DMA_IRQn           DMA1_Stream1_IRQn
#define GW_UART_RX_DMA_BUFFER_SIZE    256U
#endif

#define OTA_BL_BOOT_MAGIC             0xB00710ADUL
#define OTA_META_MAGIC                0x454F5441UL
#define OTA_META_SCHEMA_VERSION       2U
#define OTA_META_ADDR                 0x08010000UL
#define OTA_META_SECTOR               FLASH_SECTOR_4
#define OTA_SLOT_A_BASE               0x08020000UL
#define OTA_SLOT_B_BASE               0x08080000UL
#define OTA_SLOT_A                    0U
#define OTA_SLOT_B                    1U
#define OTA_SLOT_NONE                 0xFFU
#define OTA_IMG_PENDING               2U
#define OTA_IMG_CONFIRMED             3U

typedef struct
{
	uint32_t magic;
	uint16_t schema_version;
	uint16_t meta_seq;
	uint8_t active_slot;
	uint8_t pending_slot;
	uint8_t confirmed_slot;
	uint8_t boot_attempts;
	uint32_t app_a_version;
	uint32_t app_a_size;
	uint32_t app_a_crc32;
	uint32_t app_a_state;
	uint32_t app_b_version;
	uint32_t app_b_size;
	uint32_t app_b_crc32;
	uint32_t app_b_state;
	uint32_t rollback_count;
	uint32_t min_allowed_version;
	uint32_t last_error;
	uint32_t metadata_crc32;
} ota_metadata_t;

static UART_HandleTypeDef g_gateway_uart;
static volatile uint8_t g_gateway_rx_ring[GW_RX_RING_SIZE];
static volatile uint16_t g_gateway_rx_head = 0U;
static volatile uint16_t g_gateway_rx_tail = 0U;
#if defined(SH_MCU_F407)
static DMA_HandleTypeDef g_gateway_uart_rx_dma;
static uint8_t g_gateway_rx_dma_buf[GW_UART_RX_DMA_BUFFER_SIZE];
static volatile uint16_t g_gateway_rx_dma_last_pos = 0U;
static volatile uint32_t g_gateway_rx_dma_event_count = 0U;
static volatile uint32_t g_gateway_rx_ring_overflow_count = 0U;
static volatile uint32_t g_gateway_rx_dma_error_count = 0U;
#endif
static uint32_t g_report_seq = 0U;

typedef struct
{
	uint8_t active;
	uint8_t from_replay;
	uint32_t report_seq;
	uint32_t cache_seq;
	uint16_t frame_len;
	uint32_t sent_ms;
	uint8_t frame[GW_TX_FRAME_MAX_SIZE];
} PendingReport;

static PendingReport g_pending_reports[GW_PENDING_QUEUE_SIZE];
static uint8_t g_replay_active = 0U;
static uint32_t g_replay_last_try_ms = 0U;
static uint32_t g_replay_last_backlog = 0U;
static uint32_t g_tx_hex_dump_count = 0U;

static uint32_t gw_pending_count(void)
{
	uint32_t i;
	uint32_t cnt = 0U;
	for (i = 0U; i < GW_PENDING_QUEUE_SIZE; ++i)
	{
		if (g_pending_reports[i].active != 0U)
		{
			cnt++;
		}
	}
	return cnt;
}

static PendingReport *gw_pending_find_by_seq(uint32_t report_seq)
{
	uint32_t i;
	for (i = 0U; i < GW_PENDING_QUEUE_SIZE; ++i)
	{
		if (g_pending_reports[i].active != 0U && g_pending_reports[i].report_seq == report_seq)
		{
			return &g_pending_reports[i];
		}
	}
	return 0;
}

static PendingReport *gw_pending_find_replay_cache_seq(uint32_t cache_seq)
{
	uint32_t i;
	for (i = 0U; i < GW_PENDING_QUEUE_SIZE; ++i)
	{
		if (g_pending_reports[i].active != 0U &&
			g_pending_reports[i].from_replay != 0U &&
			g_pending_reports[i].cache_seq == cache_seq)
		{
			return &g_pending_reports[i];
		}
	}
	return 0;
}

static PendingReport *gw_pending_alloc_slot(void)
{
	uint32_t i;
	for (i = 0U; i < GW_PENDING_QUEUE_SIZE; ++i)
	{
		if (g_pending_reports[i].active == 0U)
		{
			return &g_pending_reports[i];
		}
	}
	return 0;
}

static void gw_pending_clear(PendingReport *slot)
{
	if (slot == 0)
	{
		return;
	}
	memset(slot, 0, sizeof(*slot));
}

static uint8_t gw_pending_enqueue(uint8_t from_replay, uint32_t cache_seq, uint32_t report_seq, const uint8_t *frame, uint16_t len, uint32_t sent_ms)
{
	PendingReport *slot = gw_pending_alloc_slot();
	if (slot == 0 || frame == 0 || len == 0U || len > GW_TX_FRAME_MAX_SIZE)
	{
		return 0U;
	}
	memset(slot, 0, sizeof(*slot));
	slot->active = 1U;
	slot->from_replay = from_replay;
	slot->cache_seq = cache_seq;
	slot->report_seq = report_seq;
	slot->frame_len = len;
	slot->sent_ms = sent_ms;
	memcpy(slot->frame, frame, len);
	return 1U;
}

static uint32_t ota_crc32(const uint8_t *data, uint32_t len)
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

static void ota_meta_update_crc(ota_metadata_t *meta)
{
	if (meta == 0)
	{
		return;
	}
	meta->metadata_crc32 = 0U;
	meta->metadata_crc32 = ota_crc32((const uint8_t *)meta, (uint32_t)sizeof(*meta));
}

static uint8_t ota_meta_seq_is_newer(uint16_t candidate, uint16_t current)
{
	uint16_t delta = (uint16_t)(candidate - current);
	return (delta != 0U && delta < 0x8000U) ? 1U : 0U;
}

static uint8_t ota_meta_slot_is_erased(uint32_t addr)
{
	const uint8_t *p = (const uint8_t *)addr;
	uint32_t i;
	for (i = 0U; i < (uint32_t)sizeof(ota_metadata_t); ++i)
	{
		if (p[i] != 0xFFU)
		{
			return 0U;
		}
	}
	return 1U;
}

static uint8_t ota_meta_load(ota_metadata_t *out)
{
	uint32_t i;
	uint8_t found = 0U;
	ota_metadata_t best;
	if (out == 0)
	{
		return 0U;
	}
	for (i = 0U; i < (0x10000UL / (uint32_t)sizeof(ota_metadata_t)); ++i)
	{
		uint32_t addr = OTA_META_ADDR + i * (uint32_t)sizeof(ota_metadata_t);
		const ota_metadata_t *rec = (const ota_metadata_t *)addr;
		ota_metadata_t tmp;
		uint32_t crc;
		if (ota_meta_slot_is_erased(addr) != 0U)
		{
			break;
		}
		tmp = *rec;
		if (tmp.magic != OTA_META_MAGIC || tmp.schema_version != OTA_META_SCHEMA_VERSION)
		{
			continue;
		}
		crc = tmp.metadata_crc32;
		tmp.metadata_crc32 = 0U;
		if (ota_crc32((const uint8_t *)&tmp, (uint32_t)sizeof(tmp)) != crc)
		{
			continue;
		}
		if (found == 0U || ota_meta_seq_is_newer(tmp.meta_seq, best.meta_seq) != 0U)
		{
			best = *rec;
			found = 1U;
		}
	}
	if (found == 0U)
	{
		return 0U;
	}
	*out = best;
	return 1U;
}

static uint8_t ota_meta_commit(const ota_metadata_t *meta)
{
	FLASH_EraseInitTypeDef erase;
	uint32_t sector_error = 0U;
	uint32_t i;
	uint32_t append_addr = 0U;
	uint8_t append_found = 0U;
	const uint8_t *src;
	const uint8_t *dst;
	ota_metadata_t work;
	if (meta == 0)
	{
		return 0U;
	}
	work = *meta;
	ota_meta_update_crc(&work);
	memset(&erase, 0, sizeof(erase));

	HAL_FLASH_Unlock();
	for (i = 0U; i < (0x10000UL / (uint32_t)sizeof(ota_metadata_t)); ++i)
	{
		uint32_t addr = OTA_META_ADDR + i * (uint32_t)sizeof(ota_metadata_t);
		if (ota_meta_slot_is_erased(addr) != 0U)
		{
			append_addr = addr;
			append_found = 1U;
			break;
		}
	}

	if (append_found == 0U)
	{
		erase.TypeErase = FLASH_TYPEERASE_SECTORS;
		erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
		erase.Sector = OTA_META_SECTOR;
		erase.NbSectors = 1U;
		if (HAL_FLASHEx_Erase(&erase, &sector_error) != HAL_OK)
		{
			HAL_FLASH_Lock();
			return 0U;
		}
		append_addr = OTA_META_ADDR;
	}

	src = (const uint8_t *)&work;
	dst = (const uint8_t *)append_addr;
	for (i = 0U; i < (uint32_t)sizeof(work); ++i)
	{
		if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE, append_addr + i, src[i]) != HAL_OK)
		{
			HAL_FLASH_Lock();
			return 0U;
		}
	}
	HAL_FLASH_Lock();
	for (i = 0U; i < (uint32_t)sizeof(work); ++i)
	{
		if (src[i] != dst[i])
		{
			return 0U;
		}
	}
	return 1U;
}

static uint8_t ota_current_slot(void)
{
	uint32_t vec = (uint32_t)&__Vectors;
	LOG_INFO("GATEWAY", "ota detect vector=0x%08lX", (unsigned long)vec);
	if (vec >= OTA_SLOT_B_BASE && vec < (OTA_SLOT_B_BASE + 0x00060000UL))
	{
		return OTA_SLOT_B;
	}
	return OTA_SLOT_A;
}

static void OtaApp_RequestBootloader(void)
{
	__HAL_RCC_PWR_CLK_ENABLE();
	HAL_PWR_EnableBkUpAccess();
	__HAL_RCC_RTC_ENABLE();
	RTC->BKP0R = OTA_BL_BOOT_MAGIC;
}

void Gateway_ConfirmSlotEarly(void)
{
	ota_metadata_t meta;
	uint8_t slot;
	if (ota_meta_load(&meta) == 0U)
	{
		LOG_WARN("GATEWAY", "ota confirm skip: meta load failed");
		return;
	}
	slot = ota_current_slot();
	if (meta.pending_slot != slot)
	{
		LOG_INFO("GATEWAY", "ota confirm skip: pending=%u current=%u",
			(unsigned int)meta.pending_slot,
			(unsigned int)slot);
		return;
	}
	if ((slot == OTA_SLOT_A && meta.app_a_state != OTA_IMG_PENDING) ||
		(slot == OTA_SLOT_B && meta.app_b_state != OTA_IMG_PENDING))
	{
		LOG_INFO("GATEWAY", "ota confirm skip: state mismatch a=%lu b=%lu slot=%u",
			(unsigned long)meta.app_a_state,
			(unsigned long)meta.app_b_state,
			(unsigned int)slot);
		return;
	}
	LOG_INFO("GATEWAY", "ota confirm begin slot=%u seq=%u", (unsigned int)slot, (unsigned int)meta.meta_seq);
	meta.active_slot = slot;
	meta.confirmed_slot = slot;
	meta.pending_slot = OTA_SLOT_NONE;
	meta.boot_attempts = 0U;
	if (slot == OTA_SLOT_A)
	{
		meta.app_a_state = OTA_IMG_CONFIRMED;
		if (meta.app_a_version > meta.min_allowed_version)
		{
			meta.min_allowed_version = meta.app_a_version;
		}
	}
	else
	{
		meta.app_b_state = OTA_IMG_CONFIRMED;
		if (meta.app_b_version > meta.min_allowed_version)
		{
			meta.min_allowed_version = meta.app_b_version;
		}
	}
	meta.meta_seq += 1U;
	if (ota_meta_commit(&meta) != 0U)
	{
		LOG_INFO("GATEWAY", "ota confirm commit ok slot=%u seq=%u min_ver=%lu",
			(unsigned int)slot,
			(unsigned int)meta.meta_seq,
			(unsigned long)meta.min_allowed_version);
	}
	else
	{
		LOG_ERROR("GATEWAY", "ota confirm commit failed slot=%u seq=%u",
			(unsigned int)slot,
			(unsigned int)meta.meta_seq);
	}
}

static uint8_t gw_can_confirm_slot(void)
{
	SmartHomeState state;
	StorageStats storage_stats;

	if (!Watchdog_IsTaskFresh(WD_TASK_GATEWAY) ||
		!Watchdog_IsTaskFresh(WD_TASK_SENSOR) ||
		!Watchdog_IsTaskFresh(WD_TASK_ALARM) ||
		!Watchdog_IsTaskFresh(WD_TASK_STORAGE) ||
		!Watchdog_IsTaskFresh(WD_TASK_DISPLAY) ||
		!Watchdog_IsTaskFresh(WD_TASK_KEY) ||
		!Watchdog_IsTaskFresh(WD_TASK_LED))
	{
		return 0U;
	}

	if (!Storage_GetStats(&storage_stats))
	{
		return 0U;
	}

	SmartHomeState_Get(&state);
	return (state.sensor_valid != 0U) ? 1U : 0U;
}

static uint16_t gw_crc16_modbus(const uint8_t *data, uint16_t len)
{
	uint16_t crc = 0xFFFFU;
	uint16_t i;
	uint8_t bit;

	for (i = 0U; i < len; i++)
	{
		crc ^= (uint16_t)data[i];
		for (bit = 0U; bit < 8U; bit++)
		{
			if ((crc & 0x0001U) != 0U)
			{
				crc = (uint16_t)((crc >> 1U) ^ 0xA001U);
			}
			else
			{
				crc >>= 1U;
			}
		}
	}
	return crc;
}

static int16_t gw_float_to_x10(float value)
{
	if (value > 3276.7f)
	{
		return 32767;
	}
	if (value < -3276.8f)
	{
		return -32768;
	}
	if (value >= 0.0f)
	{
		return (int16_t)(value * 10.0f + 0.5f);
	}
	return (int16_t)(value * 10.0f - 0.5f);
}

static uint16_t gw_float_to_u16_x10(float value)
{
	int32_t scaled;
	if (value <= 0.0f)
	{
		return 0U;
	}
	scaled = (int32_t)(value * 10.0f + 0.5f);
	if (scaled > 65535)
	{
		scaled = 65535;
	}
	return (uint16_t)scaled;
}

static int gw_uart_send_bytes(const uint8_t *data, uint16_t len)
{
	uint16_t i;
	char hex_line[3 * 32 + 1];
	uint16_t dump_n;
	uint16_t pos = 0U;
	uint32_t start_tick;
	if (data == 0 || len == 0U)
	{
		return -1;
	}
	if (g_tx_hex_dump_count < GW_TX_HEX_DUMP_MAX_FRAMES)
	{
		dump_n = (len < 32U) ? len : 32U;
		for (i = 0U; i < dump_n; ++i)
		{
			static const char hexdig[] = "0123456789ABCDEF";
			uint8_t b = data[i];
			hex_line[pos++] = hexdig[(b >> 4) & 0x0FU];
			hex_line[pos++] = hexdig[b & 0x0FU];
			if (i + 1U < dump_n)
			{
				hex_line[pos++] = ' ';
			}
		}
		hex_line[pos] = '\0';
		LOG_INFO("GW", "txhex[%lu] len=%u dump=%s",
			(unsigned long)g_tx_hex_dump_count,
			(unsigned int)len,
			hex_line);
		g_tx_hex_dump_count++;
	}

	/* Robust blocking TX: avoid frame fragmentation when HAL timeout occurs
	 * under transient scheduling/IRQ latency.
	 */
	for (i = 0U; i < len; ++i)
	{
		start_tick = HAL_GetTick();
		while ((g_gateway_uart.Instance->SR & USART_SR_TXE) == 0U)
		{
			if ((HAL_GetTick() - start_tick) > 1000U)
			{
				return -1;
			}
		}
		g_gateway_uart.Instance->DR = (uint16_t)data[i];
	}

	start_tick = HAL_GetTick();
	while ((g_gateway_uart.Instance->SR & USART_SR_TC) == 0U)
	{
		if ((HAL_GetTick() - start_tick) > 1000U)
		{
			return -1;
		}
	}

	return 0;
}

static int gw_send_frame(uint8_t type, const uint8_t *payload, uint8_t payload_len)
{
	uint8_t frame[GW_TX_FRAME_MAX_SIZE];
	uint16_t crc;
	uint16_t idx = 0U;
	uint8_t wire_len = (uint8_t)(payload_len + 1U);

	if (wire_len > (uint8_t)(GW_TX_FRAME_MAX_SIZE - 5U))
	{
		return -1;
	}

	frame[idx++] = GW_FRAME_HEAD_0;
	frame[idx++] = GW_FRAME_HEAD_1;
	frame[idx++] = wire_len;
	frame[idx++] = type;

	if (payload_len > 0U && payload != 0)
	{
		memcpy(&frame[idx], payload, payload_len);
		idx = (uint16_t)(idx + payload_len);
	}

	crc = gw_crc16_modbus(&frame[2], (uint16_t)(wire_len + 1U));
	frame[idx++] = (uint8_t)(crc & 0xFFU);
	frame[idx++] = (uint8_t)((crc >> 8U) & 0xFFU);

	if (gw_uart_send_bytes(frame, idx) == 0)
	{
		return 0;
	}
	return -1;
}

static uint8_t gw_frame_extract_report_seq(const uint8_t *frame, uint16_t len, uint32_t *out_seq)
{
	if (frame == 0 || out_seq == 0 || len < 20U)
	{
		return 0U;
	}
	if (frame[0] != GW_FRAME_HEAD_0 || frame[1] != GW_FRAME_HEAD_1 || frame[3] != GW_TYPE_SENSOR_DATA)
	{
		return 0U;
	}
	if (frame[2] < 15U)
	{
		return 0U;
	}
	/* Sensor report payload layout v2:
	 * 0:dev 1-2:temp 3-4:humi 5-6:vbat 7-8:light 9:status 10-13:report_seq
	 */
	*out_seq = (uint32_t)frame[14] |
		((uint32_t)frame[15] << 8U) |
		((uint32_t)frame[16] << 16U) |
		((uint32_t)frame[17] << 24U);
	return 1U;
}

static uint8_t gw_sensor_payload_is_abnormal(const uint8_t *payload, uint8_t payload_len)
{
	uint8_t status_bits;

	if (payload == 0 || payload_len < 10U)
	{
		return 0U;
	}

	status_bits = payload[9];
	if ((status_bits & 0x02U) != 0U)
	{
		return 1U;
	}
	if ((status_bits & 0x04U) == 0U)
	{
		return 1U;
	}
	return 0U;
}

static uint8_t gw_sensor_frame_is_abnormal(const uint8_t *frame, uint16_t len)
{
	uint8_t payload_len;

	if (frame == 0 || len < 20U)
	{
		return 0U;
	}
	if (frame[0] != GW_FRAME_HEAD_0 || frame[1] != GW_FRAME_HEAD_1 || frame[3] != GW_TYPE_SENSOR_DATA)
	{
		return 0U;
	}
	payload_len = (uint8_t)(frame[2] - 1U);
	return gw_sensor_payload_is_abnormal(&frame[4], payload_len);
}

static void gw_try_cache_abnormal_snapshot(const uint8_t *frame, uint16_t len, uint32_t report_seq)
{
	uint32_t backlog;

	if (!gw_sensor_frame_is_abnormal(frame, len))
	{
		LOG_INFO("GW", "drop normal snapshot seq=%lu while offline", (unsigned long)report_seq);
		return;
	}

	backlog = Storage_GetCacheBacklog();
	if (backlog != 0U)
	{
		LOG_WARN("GW", "abnormal snapshot skip seq=%lu backlog=%lu",
			(unsigned long)report_seq,
			(unsigned long)backlog);
		return;
	}

	if (Storage_WriteCache(frame, len))
	{
		LOG_WARN("GW", "abnormal snapshot cached seq=%lu", (unsigned long)report_seq);
	}
}

static void gw_pending_handle_timeout(PendingReport *slot)
{
	if (slot == 0 || slot->active == 0U)
	{
		return;
	}
	if (slot->from_replay == 0U)
	{
		gw_try_cache_abnormal_snapshot(slot->frame, slot->frame_len, slot->report_seq);
	}
	gw_pending_clear(slot);
}

static void gw_pending_scan_timeout(uint32_t now_ms)
{
	uint32_t i;
	for (i = 0U; i < GW_PENDING_QUEUE_SIZE; ++i)
	{
		PendingReport *slot = &g_pending_reports[i];
		if (slot->active == 0U)
		{
			continue;
		}
		if ((now_ms - slot->sent_ms) > GW_REPORT_ACK_TIMEOUT_MS)
		{
			gw_pending_handle_timeout(slot);
		}
	}
}

static void gw_process_report_ack(const uint8_t *payload, uint8_t payload_len)
{
	uint32_t ack_seq;
	PendingReport *slot;
	if (payload == 0 || payload_len < 5U)
	{
		return;
	}
	ack_seq = (uint32_t)payload[1] |
		((uint32_t)payload[2] << 8U) |
		((uint32_t)payload[3] << 16U) |
		((uint32_t)payload[4] << 24U);
	slot = gw_pending_find_by_seq(ack_seq);
	if (slot == 0)
	{
		return;
	}
	LOG_INFO("GW", "report ack seq=%lu", (unsigned long)ack_seq);
	if (slot->from_replay != 0U)
	{
		(void)Storage_MarkCacheAck(slot->cache_seq);
		LOG_INFO("GW", "replay ack cache_seq=%lu backlog=%lu",
			(unsigned long)slot->cache_seq,
			(unsigned long)Storage_GetCacheBacklog());
		if (Storage_GetCacheBacklog() == 0U)
		{
			LOG_INFO("GW", "replay done backlog=0");
			g_replay_active = 0U;
			g_replay_last_backlog = 0U;
		}
	}
	gw_pending_clear(slot);
}

static void gw_try_replay_cache(uint32_t now_ms)
{
	uint32_t backlog = Storage_GetCacheBacklog();
	uint32_t i;
	StorageCachedRecord rec;
	uint32_t report_seq;
	if (backlog == 0U)
	{
		g_replay_active = 0U;
		g_replay_last_backlog = 0U;
		return;
	}

	if ((now_ms - g_replay_last_try_ms) < GW_REPLAY_RETRY_INTERVAL_MS)
	{
		return;
	}
	g_replay_last_try_ms = now_ms;

	if (g_replay_active == 0U || g_replay_last_backlog != backlog)
	{
		LOG_INFO("GW", "replay start backlog=%lu", (unsigned long)backlog);
		g_replay_active = 1U;
		g_replay_last_backlog = backlog;
	}

	for (i = 0U; i < GW_REPLAY_BATCH_MAX; ++i)
	{
		if (gw_pending_count() >= GW_PENDING_QUEUE_SIZE)
		{
			LOG_WARN("GW", "replay paused pending full pending=%lu backlog=%lu",
				(unsigned long)gw_pending_count(),
				(unsigned long)backlog);
			break;
		}
		if (!Storage_PeekCache(&rec))
		{
			LOG_WARN("GW", "replay peek failed backlog=%lu pending=%lu",
				(unsigned long)backlog,
				(unsigned long)gw_pending_count());
			break;
		}
		if (gw_pending_find_replay_cache_seq(rec.seq) != 0)
		{
			/* Head cache record is already in-flight, wait for ack/timeout. */
			break;
		}
		if (gw_frame_extract_report_seq(rec.payload, rec.len, &report_seq) == 0U)
		{
			(void)Storage_MarkCacheAck(rec.seq);
			continue;
		}
		if (gw_uart_send_bytes(rec.payload, rec.len) != 0)
		{
			break;
		}
		if (gw_pending_enqueue(1U, rec.seq, report_seq, rec.payload, rec.len, now_ms) == 0U)
		{
			LOG_WARN("GW", "pending full drop replay cache_seq=%lu", (unsigned long)rec.seq);
			break;
		}
		LOG_INFO("GW", "replay send cache_seq=%lu report_seq=%lu",
			(unsigned long)rec.seq,
			(unsigned long)report_seq);
	}
}

static void gw_send_sensor_data(void)
{
	SmartHomeState state;
	int16_t temp_x10;
	uint16_t humi_x10;
	uint16_t light_raw;
	uint8_t payload[14];
	uint8_t status_bits = 0U;
	uint8_t frame[GW_TX_FRAME_MAX_SIZE];
	uint16_t crc;
	uint16_t idx = 0U;
	uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

	SmartHomeState_Get(&state);
	temp_x10 = gw_float_to_x10(state.temperature);
	humi_x10 = gw_float_to_u16_x10(state.humidity);
	light_raw = state.light;

	if (state.led_on != 0U)
	{
		status_bits |= 0x01U;
	}
	if (state.alarm_on != 0U)
	{
		status_bits |= 0x02U;
	}
	if (state.sensor_valid != 0U)
	{
		status_bits |= 0x04U;
	}
	if (state.mode == SMARTHOME_MODE_AUTO)
	{
		status_bits |= 0x08U;
	}

	payload[0] = (uint8_t)GW_DEVICE_ID;
	payload[1] = (uint8_t)(temp_x10 & 0xFF);
	payload[2] = (uint8_t)((temp_x10 >> 8) & 0xFF);
	payload[3] = (uint8_t)(humi_x10 & 0xFF);
	payload[4] = (uint8_t)((humi_x10 >> 8) & 0xFF);
	payload[5] = (uint8_t)(GW_VOLTAGE_MV_DEFAULT & 0xFFU);
	payload[6] = (uint8_t)((GW_VOLTAGE_MV_DEFAULT >> 8U) & 0xFFU);
	payload[7] = (uint8_t)(light_raw & 0xFFU);
	payload[8] = (uint8_t)((light_raw >> 8U) & 0xFFU);
	payload[9] = status_bits;
	g_report_seq++;
	payload[10] = (uint8_t)(g_report_seq & 0xFFU);
	payload[11] = (uint8_t)((g_report_seq >> 8U) & 0xFFU);
	payload[12] = (uint8_t)((g_report_seq >> 16U) & 0xFFU);
	payload[13] = (uint8_t)((g_report_seq >> 24U) & 0xFFU);

	gw_pending_scan_timeout(now_ms);

	frame[idx++] = GW_FRAME_HEAD_0;
	frame[idx++] = GW_FRAME_HEAD_1;
	frame[idx++] = (uint8_t)(sizeof(payload) + 1U);
	frame[idx++] = GW_TYPE_SENSOR_DATA;
	memcpy(&frame[idx], payload, sizeof(payload));
	idx = (uint16_t)(idx + sizeof(payload));
	crc = gw_crc16_modbus(&frame[2], (uint16_t)(sizeof(payload) + 2U));
	frame[idx++] = (uint8_t)(crc & 0xFFU);
	frame[idx++] = (uint8_t)((crc >> 8U) & 0xFFU);

	if (gw_uart_send_bytes(frame, idx) != 0)
	{
		gw_try_cache_abnormal_snapshot(frame, idx, g_report_seq);
		return;
	}
	if (gw_pending_enqueue(0U, 0U, g_report_seq, frame, idx, now_ms) == 0U)
	{
		LOG_WARN("GW", "pending full seq=%lu abnormal-only cache policy", (unsigned long)g_report_seq);
		gw_try_cache_abnormal_snapshot(frame, idx, g_report_seq);
		return;
	}
	LOG_INFO("GW", "report send seq=%lu", (unsigned long)g_report_seq);
}

static void gw_send_heartbeat(void)
{
	uint8_t payload[1];
	payload[0] = (uint8_t)GW_DEVICE_ID;
	(void)gw_send_frame(GW_TYPE_HEARTBEAT, payload, sizeof(payload));
}

static void gw_send_alarm_event(uint8_t event_code)
{
	uint8_t payload[2];

	payload[0] = (uint8_t)GW_DEVICE_ID;
	payload[1] = event_code;
	(void)gw_send_frame(GW_TYPE_ALARM_EVENT, payload, sizeof(payload));
}

static void gw_send_device_status(void)
{
	uint8_t payload[5];
	payload[0] = (uint8_t)GW_DEVICE_ID;
	payload[1] = 1U;
	payload[2] = GW_APP_VERSION_MAJOR;
	payload[3] = GW_APP_VERSION_MINOR;
	payload[4] = GW_APP_VERSION_PATCH;
	(void)gw_send_frame(GW_TYPE_DEVICE_STATUS, payload, sizeof(payload));
}

static void gw_send_app_confirm(void)
{
	uint8_t payload[5];
	payload[0] = (uint8_t)GW_DEVICE_ID;
	payload[1] = GW_APP_VERSION_MAJOR;
	payload[2] = GW_APP_VERSION_MINOR;
	payload[3] = GW_APP_VERSION_PATCH;
	payload[4] = 1U;
	(void)gw_send_frame(GW_TYPE_APP_CONFIRM, payload, sizeof(payload));
}

static void gw_send_command_ack(uint16_t command_id, uint8_t result)
{
	uint8_t payload[4];
	payload[0] = (uint8_t)GW_DEVICE_ID;
	payload[1] = (uint8_t)(command_id & 0xFFU);
	payload[2] = (uint8_t)((command_id >> 8U) & 0xFFU);
	payload[3] = result;
	(void)gw_send_frame(GW_TYPE_COMMAND_ACK, payload, sizeof(payload));
}

static void gw_handle_command_req(const uint8_t *payload, uint8_t payload_len)
{
	uint8_t target_device;
	uint16_t command_id;
	uint8_t cmd_type;
	uint8_t result = GW_ACK_OK;
	uint8_t cmd_arg = 0U;
	float temp_threshold;
	float humi_threshold;

	if (payload == 0 || payload_len < 4U)
	{
		return;
	}

	target_device = payload[0];
	if (target_device != (uint8_t)GW_DEVICE_ID)
	{
		return;
	}

	command_id = (uint16_t)payload[1] | ((uint16_t)payload[2] << 8U);
	cmd_type = payload[3];
	if (payload_len >= 5U)
	{
		cmd_arg = payload[4];
	}

	switch (cmd_type)
	{
		case GW_CMD_SET_LED:
			{
				if (payload_len < 5U)
				{
					result = GW_ACK_BAD_PAYLOAD;
					break;
				}
				SmartHomeState_SetManualLed((cmd_arg != 0U) ? 1U : 0U);
				Gateway_RequestImmediateReport();
				LOG_INFO("GATEWAY", "cmd set_led=%u", (unsigned int)((cmd_arg != 0U) ? 1U : 0U));
				break;
			}
		case GW_CMD_SET_BUZZER:
		{
			if (payload_len < 5U)
				{
					result = GW_ACK_BAD_PAYLOAD;
					break;
				}
				SmartHomeState_SetManualBuzzer((cmd_arg != 0U) ? 1U : 0U);
				Gateway_RequestImmediateReport();
				LOG_INFO("GATEWAY", "cmd set_buzzer=%u", (unsigned int)((cmd_arg != 0U) ? 1U : 0U));
				break;
			}
		case GW_CMD_SET_MODE:
		{
			if (payload_len < 5U)
			{
				result = GW_ACK_BAD_PAYLOAD;
				break;
			}
			SmartHomeState_SetMode((cmd_arg != 0U) ? SMARTHOME_MODE_AUTO : SMARTHOME_MODE_MANUAL);
			Gateway_RequestImmediateReport();
			LOG_INFO("GATEWAY", "cmd set_mode=%s", (cmd_arg != 0U) ? "auto" : "manual");
			break;
		}
		case GW_CMD_GET_STATUS:
		{
			Gateway_RequestImmediateReport();
			LOG_INFO("GATEWAY", "cmd get_status");
			break;
		}
		case GW_CMD_SET_THRESHOLD:
		{
			if (payload_len < 8U)
			{
				result = GW_ACK_BAD_PAYLOAD;
				break;
			}
			temp_threshold = (float)((uint16_t)payload[4] | ((uint16_t)payload[5] << 8U)) / 10.0f;
			humi_threshold = (float)((uint16_t)payload[6] | ((uint16_t)payload[7] << 8U)) / 10.0f;
			SmartHomeConfig_SetThreshold(temp_threshold, humi_threshold);
			Gateway_RequestImmediateReport();
			LOG_INFO("GATEWAY", "cmd set_threshold t=%.1f h=%.1f", temp_threshold, humi_threshold);
			break;
		}
		case GW_CMD_SET_LOG_LEVEL:
		{
			if (payload_len < 5U)
			{
				result = GW_ACK_BAD_PAYLOAD;
				break;
			}
			if (cmd_arg > (uint8_t)LOG_LEVEL_INFO)
			{
				result = GW_ACK_BAD_PAYLOAD;
				break;
			}
			Log_SetLevel((LogLevel)cmd_arg);
			LOG_PRINT("INFO", "GATEWAY", "cmd set_log_level=%s", Log_LevelToString(Log_GetLevel()));
			break;
		}
		case GW_CMD_REBOOT_TO_BOOTLOADER:
		{
			LOG_WARN("GATEWAY", "cmd reboot_to_bootloader");
			gw_send_command_ack(command_id, GW_ACK_OK);
			HAL_Delay(50);
			OtaApp_RequestBootloader();
			HAL_Delay(10);
			PlatformBackup_Write(BKP_IDX_SW_RESET_SRC, (uint32_t)SW_RESET_SRC_GATEWAY_CMD);
			NVIC_SystemReset();
			return;
		}
		default:
		{
			result = GW_ACK_UNKNOWN_CMD;
			LOG_WARN("GATEWAY", "unknown cmd type=%u", (unsigned int)cmd_type);
			break;
		}
	}

	gw_send_command_ack(command_id, result);
	if (result == GW_ACK_OK)
	{
		gw_send_sensor_data();
		gw_send_device_status();
	}
}

static void gw_process_frame(uint8_t frame_type, const uint8_t *payload, uint8_t payload_len)
{
	if (frame_type == GW_TYPE_COMMAND_REQ)
	{
		gw_handle_command_req(payload, payload_len);
	}
	else if (frame_type == GW_TYPE_REPORT_ACK)
	{
		gw_process_report_ack(payload, payload_len);
	}
}

#if defined(SH_MCU_F407)
static void gw_uart_rx_ring_push_isr(uint8_t byte)
{
	uint16_t next = (uint16_t)((g_gateway_rx_head + 1U) % GW_RX_RING_SIZE);
	if (next == g_gateway_rx_tail)
	{
		g_gateway_rx_ring_overflow_count++;
		return;
	}

	g_gateway_rx_ring[g_gateway_rx_head] = byte;
	g_gateway_rx_head = next;
}

static void gw_uart_dma_consume_to(uint16_t pos)
{
	while (g_gateway_rx_dma_last_pos != pos)
	{
		gw_uart_rx_ring_push_isr(g_gateway_rx_dma_buf[g_gateway_rx_dma_last_pos]);
		g_gateway_rx_dma_last_pos++;
		if (g_gateway_rx_dma_last_pos >= GW_UART_RX_DMA_BUFFER_SIZE)
		{
			g_gateway_rx_dma_last_pos = 0U;
		}
	}
}

static int gw_uart_dma_rx_start(void)
{
	g_gateway_rx_dma_last_pos = 0U;
	if (HAL_UARTEx_ReceiveToIdle_DMA(&g_gateway_uart, g_gateway_rx_dma_buf, GW_UART_RX_DMA_BUFFER_SIZE) != HAL_OK)
	{
		return -1;
	}
	return 0;
}
#endif

static void gw_process_rx_byte(uint8_t byte)
{
	static uint8_t frame_buf[GW_FRAME_BUF_SIZE];
	static uint16_t frame_len = 0U;
	uint8_t wire_len;
	uint16_t total_len;
	uint16_t wire_crc;
	uint16_t calc_crc;

	if (frame_len >= GW_FRAME_BUF_SIZE)
	{
		frame_len = 0U;
	}
	frame_buf[frame_len++] = byte;

	while (frame_len >= 5U)
	{
		if (frame_buf[0] != GW_FRAME_HEAD_0 || frame_buf[1] != GW_FRAME_HEAD_1)
		{
			memmove(frame_buf, &frame_buf[1], frame_len - 1U);
			frame_len--;
			continue;
		}

		wire_len = frame_buf[2];
		if (wire_len < 1U || wire_len > (GW_FRAME_BUF_SIZE - 5U))
		{
			memmove(frame_buf, &frame_buf[1], frame_len - 1U);
			frame_len--;
			continue;
		}

		total_len = (uint16_t)(2U + 1U + wire_len + 2U);
		if (frame_len < total_len)
		{
			break;
		}

		wire_crc = (uint16_t)frame_buf[total_len - 2U] | ((uint16_t)frame_buf[total_len - 1U] << 8U);
		calc_crc = gw_crc16_modbus(&frame_buf[2], (uint16_t)(wire_len + 1U));
		if (wire_crc == calc_crc)
		{
			gw_process_frame(frame_buf[3], &frame_buf[4], (uint8_t)(wire_len - 1U));
		}

		if (frame_len > total_len)
		{
			memmove(frame_buf, &frame_buf[total_len], frame_len - total_len);
		}
		frame_len = (uint16_t)(frame_len - total_len);
	}
}

static uint8_t gw_uart_read_byte(uint8_t *out)
{
	uint8_t ok = 0U;
	taskENTER_CRITICAL();
	if (g_gateway_rx_tail != g_gateway_rx_head)
	{
		*out = g_gateway_rx_ring[g_gateway_rx_tail];
		g_gateway_rx_tail = (uint16_t)((g_gateway_rx_tail + 1U) % GW_RX_RING_SIZE);
		ok = 1U;
	}
	taskEXIT_CRITICAL();
	return ok;
}

static void Gateway_UART_Init(void)
{
	BOARD_ENABLE_GW_UART_CLK();
	if (BOARD_GW_TX_PORT == GPIOA || BOARD_GW_RX_PORT == GPIOA)
	{
		BOARD_ENABLE_GPIOA_CLK();
	}
	if (BOARD_GW_TX_PORT == GPIOB || BOARD_GW_RX_PORT == GPIOB)
	{
		BOARD_ENABLE_GPIOB_CLK();
	}
	Board_UART_GPIO_Init_TX(BOARD_GW_TX_PORT, BOARD_GW_TX_PIN, BOARD_GW_UART_AF);
	Board_UART_GPIO_Init_RX(BOARD_GW_RX_PORT, BOARD_GW_RX_PIN, BOARD_GW_UART_AF);

	g_gateway_uart.Instance = BOARD_GW_UART_INSTANCE;
	g_gateway_uart.Init.BaudRate = GW_UART_BAUDRATE;
	g_gateway_uart.Init.WordLength = UART_WORDLENGTH_8B;
	g_gateway_uart.Init.StopBits = UART_STOPBITS_1;
	g_gateway_uart.Init.Parity = UART_PARITY_NONE;
	g_gateway_uart.Init.Mode = UART_MODE_TX_RX;
	g_gateway_uart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
	g_gateway_uart.Init.OverSampling = UART_OVERSAMPLING_16;
	(void)HAL_UART_Init(&g_gateway_uart);

#if defined(SH_MCU_F407)
	__HAL_RCC_DMA1_CLK_ENABLE();
	g_gateway_uart_rx_dma.Instance = GW_UART_RX_DMA_INSTANCE;
	g_gateway_uart_rx_dma.Init.Channel = GW_UART_RX_DMA_CHANNEL;
	g_gateway_uart_rx_dma.Init.Direction = DMA_PERIPH_TO_MEMORY;
	g_gateway_uart_rx_dma.Init.PeriphInc = DMA_PINC_DISABLE;
	g_gateway_uart_rx_dma.Init.MemInc = DMA_MINC_ENABLE;
	g_gateway_uart_rx_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
	g_gateway_uart_rx_dma.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
	g_gateway_uart_rx_dma.Init.Mode = DMA_CIRCULAR;
	g_gateway_uart_rx_dma.Init.Priority = DMA_PRIORITY_HIGH;
	g_gateway_uart_rx_dma.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
	g_gateway_uart_rx_dma.Init.FIFOThreshold = DMA_FIFO_THRESHOLD_FULL;
	g_gateway_uart_rx_dma.Init.MemBurst = DMA_MBURST_SINGLE;
	g_gateway_uart_rx_dma.Init.PeriphBurst = DMA_PBURST_SINGLE;
	__HAL_LINKDMA(&g_gateway_uart, hdmarx, g_gateway_uart_rx_dma);
	(void)HAL_DMA_DeInit(&g_gateway_uart_rx_dma);
	(void)HAL_DMA_Init(&g_gateway_uart_rx_dma);

	HAL_NVIC_SetPriority(GW_UART_RX_DMA_IRQn, 1, 0);
	HAL_NVIC_EnableIRQ(GW_UART_RX_DMA_IRQn);

	if (gw_uart_dma_rx_start() != 0)
	{
		LOG_ERROR("GATEWAY", "USART3 RX DMA start failed");
	}
#else
	__HAL_UART_ENABLE_IT(&g_gateway_uart, UART_IT_RXNE);
#endif
	HAL_NVIC_SetPriority(BOARD_GW_UART_IRQn, 1, 1);
	HAL_NVIC_EnableIRQ(BOARD_GW_UART_IRQn);
}

void BOARD_GW_UART_IRQ_HANDLER(void)
{
#if defined(SH_MCU_F407)
	HAL_UART_IRQHandler(&g_gateway_uart);
#else
	uint8_t rx_data;
	uint16_t next;

	if (__HAL_UART_GET_FLAG(&g_gateway_uart, UART_FLAG_RXNE) == SET)
	{
		__HAL_UART_CLEAR_FLAG(&g_gateway_uart, UART_FLAG_RXNE);
		rx_data = (uint8_t)(g_gateway_uart.Instance->DR & 0xFFU);
		next = (uint16_t)((g_gateway_rx_head + 1U) % GW_RX_RING_SIZE);
		if (next != g_gateway_rx_tail)
		{
			g_gateway_rx_ring[g_gateway_rx_head] = rx_data;
			g_gateway_rx_head = next;
		}
	}
#endif
}

#if defined(SH_MCU_F407)
void DMA1_Stream1_IRQHandler(void)
{
	HAL_DMA_IRQHandler(&g_gateway_uart_rx_dma);
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
	uint16_t pos = Size;

	if (huart != &g_gateway_uart)
	{
		return;
	}
	if (pos > GW_UART_RX_DMA_BUFFER_SIZE)
	{
		pos = GW_UART_RX_DMA_BUFFER_SIZE;
	}

	g_gateway_rx_dma_event_count++;
	gw_uart_dma_consume_to(pos);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
	if (huart != &g_gateway_uart)
	{
		return;
	}

	g_gateway_rx_dma_error_count++;
	(void)HAL_UART_AbortReceive(huart);
	(void)gw_uart_dma_rx_start();
}
#endif

static void GatewayTask(void *pvParameters)
{
	SmartHomeRuntimeConfig runtime_cfg;
	TickType_t last_report_tick;
	TickType_t last_heartbeat_tick;
	TickType_t last_diag_tick;
	TickType_t boot_tick;
	uint8_t rx_byte;
	uint8_t event_code;

	(void)pvParameters;
	Gateway_UART_Init();
	SmartHomeState_SetWiFiConnected(0U);
	SmartHomeState_SetMQTTConnected(0U);
	SmartHomeState_SetMQTTReconnectFailCount(0U);

	last_report_tick = xTaskGetTickCount();
	last_heartbeat_tick = xTaskGetTickCount();
	last_diag_tick = xTaskGetTickCount();
	boot_tick = xTaskGetTickCount();
	g_gateway_force_report = 1U;
	g_app_confirm_sent = 0U;
	memset(g_pending_reports, 0, sizeof(g_pending_reports));
	g_replay_active = 0U;
	g_replay_last_try_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
	g_replay_last_backlog = 0U;

	LOG_INFO("GATEWAY", "gateway serial task started on USART3");

	while (1)
	{
		Watchdog_Kick(WD_TASK_GATEWAY);
		while (gw_uart_read_byte(&rx_byte) != 0U)
		{
			gw_process_rx_byte(rx_byte);
		}
		while (g_gateway_event_queue != NULL &&
			xQueueReceive(g_gateway_event_queue, &event_code, 0U) == pdTRUE)
		{
			gw_send_alarm_event(event_code);
		}

		SmartHomeConfig_GetRuntime(&runtime_cfg);
		if (g_gateway_force_report != 0U ||
			(xTaskGetTickCount() - last_report_tick >= pdMS_TO_TICKS(runtime_cfg.mqtt_report_period_ms)))
		{
			TickType_t now_tick = xTaskGetTickCount();
			uint32_t report_delta_ms = (uint32_t)((now_tick - last_report_tick) * portTICK_PERIOD_MS);
			gw_send_sensor_data();
			last_report_tick = now_tick;
			g_gateway_force_report = 0U;
			if (report_delta_ms > (uint32_t)(runtime_cfg.mqtt_report_period_ms + 300U))
			{
				LOG_WARN("GATEWAY", "sensor report jitter=%lu ms target=%u ms",
					(unsigned long)report_delta_ms,
					(unsigned int)runtime_cfg.mqtt_report_period_ms);
			}
		}

		if (xTaskGetTickCount() - last_heartbeat_tick >= pdMS_TO_TICKS(GW_HEARTBEAT_PERIOD_MS))
		{
			gw_send_heartbeat();
			gw_send_device_status();
			last_heartbeat_tick = xTaskGetTickCount();
		}

		gw_try_replay_cache(xTaskGetTickCount() * portTICK_PERIOD_MS);

		if (xTaskGetTickCount() - last_diag_tick >= pdMS_TO_TICKS(GW_DIAG_HEARTBEAT_MS))
		{
#if defined(SH_MCU_F407)
			LOG_INFO("GATEWAY", "diag stack_hwm=%u report_ms=%u hb_ms=%u pending=%lu rx_dma_evt=%lu rx_ovf=%lu rx_err=%lu",
				(unsigned int)uxTaskGetStackHighWaterMark(NULL),
				(unsigned int)runtime_cfg.mqtt_report_period_ms,
				(unsigned int)GW_HEARTBEAT_PERIOD_MS,
				(unsigned long)gw_pending_count(),
				(unsigned long)g_gateway_rx_dma_event_count,
				(unsigned long)g_gateway_rx_ring_overflow_count,
				(unsigned long)g_gateway_rx_dma_error_count);
#else
			LOG_INFO("GATEWAY", "diag stack_hwm=%u report_ms=%u hb_ms=%u pending=%lu",
				(unsigned int)uxTaskGetStackHighWaterMark(NULL),
				(unsigned int)runtime_cfg.mqtt_report_period_ms,
				(unsigned int)GW_HEARTBEAT_PERIOD_MS,
				(unsigned long)gw_pending_count());
#endif
			last_diag_tick = xTaskGetTickCount();
		}

		if (g_app_confirm_sent == 0U &&
			(xTaskGetTickCount() - boot_tick) >= pdMS_TO_TICKS(GW_APP_CONFIRM_DELAY_MS))
		{
			if (gw_can_confirm_slot() != 0U)
			{
				Gateway_ConfirmSlotEarly();
				gw_send_app_confirm();
				g_app_confirm_sent = 1U;
				LOG_INFO("GATEWAY", "app confirm sent version=%u.%u.%u",
					(unsigned int)GW_APP_VERSION_MAJOR,
					(unsigned int)GW_APP_VERSION_MINOR,
					(unsigned int)GW_APP_VERSION_PATCH);
			}
			else
			{
				LOG_WARN("GATEWAY", "app confirm deferred waiting for subsystem health");
			}
		}
		FaultDiag_TestInjectOnce();

		vTaskDelay(pdMS_TO_TICKS(10U));
	}
}

void vStartGatewayTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority)
{
	BaseType_t x = 0L;
	if (g_gateway_event_queue == NULL)
	{
		g_gateway_event_queue = xQueueCreate(GW_EVENT_QUEUE_LEN, sizeof(uint8_t));
		if (g_gateway_event_queue == NULL)
		{
			LOG_ERROR("GATEWAY", "create event queue failed");
			return;
		}
	}
	if (xTaskCreate(
			GatewayTask,
			"Gateway",
			usTaskStackSize,
			(void *)x,
			uxTaskPriority,
			NULL) == pdPASS)
	{
		LOG_INFO("GATEWAY", "create task success");
	}
	else
	{
		LOG_ERROR("GATEWAY", "create task failed");
	}
}



