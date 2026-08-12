#include "app_watchdog.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "config.h"
#include "log.h"
#include "board_profile.h"
#include "platform_backup.h"

typedef struct
{
	uint8_t used;
	const char *name;
	uint32_t timeout_ms;
	uint32_t last_kick_ms;
} WatchdogEntry;

static WatchdogEntry g_wd[WD_TASK_MAX];
static uint8_t g_iwdg_enabled = 0U;

static void iwdg_hw_init(void)
{
#if defined(SH_MCU_F407)
	IWDG->KR = 0x5555U;
	IWDG->PR = 6U;
	IWDG->RLR = 750U;
	IWDG->KR = 0xAAAAU;
	IWDG->KR = 0xCCCCU;
	DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_IWDG_STOP;
#endif
}

static void iwdg_hw_feed(void)
{
#if defined(SH_MCU_F407)
	IWDG->KR = 0xAAAAU;
#endif
}

bool Watchdog_IsTaskFresh(WatchdogTaskId id)
{
	uint32_t now;

	if (id >= WD_TASK_MAX || g_wd[id].used == 0U)
	{
		return false;
	}

	now = xTaskGetTickCount() * portTICK_PERIOD_MS;
	return ((now - g_wd[id].last_kick_ms) <= g_wd[id].timeout_ms) ? true : false;
}

void Watchdog_Init(void)
{
	memset(g_wd, 0, sizeof(g_wd));
	g_iwdg_enabled = (ENABLE_IWDG != 0) ? 1U : 0U;
	if (g_iwdg_enabled)
	{
		iwdg_hw_init();
	}
	LOG_INFO("WDG", "init timeout=6000ms enabled=%u", (unsigned int)g_iwdg_enabled);
}

void Watchdog_RegisterTask(WatchdogTaskId id, const char *name, uint32_t timeout_ms)
{
	if (id >= WD_TASK_MAX)
	{
		return;
	}
	g_wd[id].used = 1U;
	g_wd[id].name = name;
	g_wd[id].timeout_ms = timeout_ms;
	g_wd[id].last_kick_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
}

void Watchdog_Kick(WatchdogTaskId id)
{
	if (id >= WD_TASK_MAX || g_wd[id].used == 0U)
	{
		return;
	}
	g_wd[id].last_kick_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static void WatchdogTask(void *arg)
{
	(void)arg;
	while (1)
	{
		uint8_t all_ok = 1U;
		uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
		uint32_t i;
		for (i = 0U; i < WD_TASK_MAX; ++i)
		{
			if (g_wd[i].used == 0U)
			{
				continue;
			}
			if ((now - g_wd[i].last_kick_ms) > g_wd[i].timeout_ms)
			{
				all_ok = 0U;
				PlatformBackup_Write(BKP_IDX_LAST_DEAD_TASK, i);
				LOG_ERROR("WDG", "task %s timeout last=%lu now=%lu",
					(g_wd[i].name != 0) ? g_wd[i].name : "unknown",
					(unsigned long)g_wd[i].last_kick_ms,
					(unsigned long)now);
				break;
			}
		}

		if (all_ok)
		{
			if (g_iwdg_enabled)
			{
				iwdg_hw_feed();
			}
		}
		else
		{
			LOG_ERROR("WDG", "stop feeding iwdg, wait reset");
			PlatformBackup_Write(BKP_IDX_SW_RESET_SRC, (uint32_t)SW_RESET_SRC_WATCHDOG);
			if (g_iwdg_enabled)
			{
				volatile uint32_t spin = 10000000UL;
				while (spin-- > 0U)
				{
					__NOP();
				}
			}
			NVIC_SystemReset();
		}
		vTaskDelay(pdMS_TO_TICKS(200U));
	}
}

void WatchdogTask_Start(uint16_t stack_size, uint32_t priority)
{
	(void)xTaskCreate(WatchdogTask, "Watchdog", stack_size, NULL, priority, NULL);
}
