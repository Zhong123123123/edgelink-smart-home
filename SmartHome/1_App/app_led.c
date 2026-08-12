#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

#include "dev_io.h"
#include "log.h"
#include "smarthome_state.h"
#include "app_watchdog.h"

TaskHandle_t ledTaskHandle;

void LedTask(void *parameter)
{
	uint32_t notify_value = 0;
	ptIODev ledDev = IODev_GetDev(LED);
	if(ledDev != NULL && ledDev->Init != NULL && ledDev->write != NULL)
	{
		ledDev->Init(ledDev);
	}
	else
	{
		LOG_ERROR("LED", "device invalid dev=%p init=%p write=%p",
			(void *)ledDev,
			(ledDev != NULL) ? (void *)ledDev->Init : NULL,
			(ledDev != NULL) ? (void *)ledDev->write : NULL);
		ledDev = NULL;
	}
	while(1)
	{
		Watchdog_Kick(WD_TASK_LED);
		if(xTaskNotifyWait(0, 0xFFFFFFFF, &notify_value, pdMS_TO_TICKS(10)) == pdTRUE)
		{
			LOG_INFO("LED", "notify=%u", (unsigned int)notify_value);
			if (ledDev == NULL)
			{
				LOG_WARN("LED", "drop notify because device unavailable");
				continue;
			}
			ledDev->write(ledDev, (uint8_t*)&notify_value, 1);
			SmartHomeState_SetLed((uint8_t)notify_value);
		}
	}
}

void vStartLEDTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority)
{
	BaseType_t x = 0L;

	if(xTaskCreate(LedTask,		/* The function that implements the task. */
			"LED",				/* Just a text name for the task to aid debugging. */
			usTaskStackSize,	/* The stack size is defined in FreeRTOSIPConfig.h. */
			(void *)x,			/* The task parameter, not used in this case. */
			uxTaskPriority,		/* The priority assigned to the task is defined in FreeRTOSConfig.h. */
			&ledTaskHandle) == pdPASS)	/* The task handle is not used. */
			{
				LOG_INFO("LED", "create task success");
			}
	else
	{
		LOG_ERROR("LED", "create task failed");
	}
}








