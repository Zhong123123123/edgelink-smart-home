#include "FreeRTOS.h"
#include "task.h"
#include "app_gateway.h"
#include "dev_io.h"
#include "log.h"
#include "smarthome_state.h"
#include "app_watchdog.h"
#include <stdio.h>

#define KEY_MODE_SWITCH_PRESS_MS 2000U

TaskHandle_t keyTaskHandle;

void KeyTask(void *parameter)
{
	KeyEvent key = {0};
	ptIODev keyDev = IODev_GetDev(KEY);
	if(keyDev != NULL && keyDev->Init != NULL && keyDev->Read != NULL)
	{
		keyDev->Init(keyDev);
	}
	else
	{
		LOG_ERROR("KEY", "device invalid dev=%p init=%p read=%p",
			(void *)keyDev,
			(keyDev != NULL) ? (void *)keyDev->Init : NULL,
			(keyDev != NULL) ? (void *)keyDev->Read : NULL);
		keyDev = NULL;
	}
	
	while(1)
	{
		Watchdog_Kick(WD_TASK_KEY);
		if(keyDev == NULL)
		{
			vTaskDelay(pdMS_TO_TICKS(1000U));
			continue;
		}
		if(keyDev->Read(keyDev, (uint8_t*)&key, sizeof(KeyEvent)) == 0)
		{
			LOG_INFO("KEY", "press time=%u ms", (unsigned int)key.time);

			if (key.time >= KEY_MODE_SWITCH_PRESS_MS)
			{
				SmartHomeState state;
				uint8_t new_mode;

				SmartHomeState_Get(&state);
				new_mode = (state.mode == SMARTHOME_MODE_AUTO) ? SMARTHOME_MODE_MANUAL : SMARTHOME_MODE_AUTO;
				SmartHomeState_SetMode(new_mode);
				LOG_INFO("KEY", "mode switched by long press, mode=%s", new_mode == SMARTHOME_MODE_AUTO ? "auto" : "manual");
				Gateway_RequestImmediateReport();
			}
		}
		vTaskDelay(pdMS_TO_TICKS(1U));
	}
}

void vStartKeyTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority)
{
	BaseType_t x = 0L;

	if(xTaskCreate(KeyTask,		/* The function that implements the task. */
			"Key",				/* Just a text name for the task to aid debugging. */
			usTaskStackSize,	/* The stack size is defined in FreeRTOSIPConfig.h. */
			(void *)x,			/* The task parameter, not used in this case. */
			uxTaskPriority,		/* The priority assigned to the task is defined in FreeRTOSConfig.h. */
			&keyTaskHandle) == pdPASS)	/* The task handle is not used. */
			{
				LOG_INFO("KEY", "create task success");
			}
	else
	{
		LOG_ERROR("KEY", "create task failed");
	}
}








