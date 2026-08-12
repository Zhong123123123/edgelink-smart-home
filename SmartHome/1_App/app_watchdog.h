#ifndef __APP_WATCHDOG_H
#define __APP_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
	WD_TASK_GATEWAY = 0,
	WD_TASK_SENSOR,
	WD_TASK_ALARM,
	WD_TASK_STORAGE,
	WD_TASK_DISPLAY,
	WD_TASK_KEY,
	WD_TASK_LED,
	WD_TASK_MAX
} WatchdogTaskId;

void Watchdog_Init(void);
void Watchdog_RegisterTask(WatchdogTaskId id, const char *name, uint32_t timeout_ms);
void Watchdog_Kick(WatchdogTaskId id);
bool Watchdog_IsTaskFresh(WatchdogTaskId id);
void WatchdogTask_Start(uint16_t stack_size, uint32_t priority);

#endif
