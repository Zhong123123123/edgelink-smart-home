#include "main.h"

#include "config.h"
#include "app_gateway.h"
#include "app_alarm.h"
#include "app_display.h"
#include "app_sensor.h"
#include "app_storage.h"
#include "app_watchdog.h"
#include "dev_io.h"
#include "dev_net.h"
#include "fault_diag.h"
#include "log.h"
#include "smarthome_state.h"

#include "stdio.h"
#include "string.h"

#include "FreeRTOS.h"
#include "task.h"

#define APP_BUILD_TAG "20260521_force_verify_v2"

static void EarlyUart1_Init(void)
{
	/* PA9 -> USART1_TX (AF7), 115200 8N1 */
	RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
	RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

	GPIOA->MODER &= ~(3UL << (9U * 2U));
	GPIOA->MODER |= (2UL << (9U * 2U));
	GPIOA->OSPEEDR |= (3UL << (9U * 2U));
	GPIOA->AFR[1] &= ~(0xFUL << ((9U - 8U) * 4U));
	GPIOA->AFR[1] |= (7UL << ((9U - 8U) * 4U));

	USART1->CR1 = 0U;
	USART1->CR2 = 0U;
	USART1->CR3 = 0U;
	USART1->BRR = (uint16_t)((16000000UL + (115200UL / 2UL)) / 115200UL);
	USART1->CR1 = USART_CR1_TE | USART_CR1_UE;
}

static void EarlyUart1_ReconfigAfterClock(void)
{
	/* APB2 after SystemClock_Config on F407 is expected 84MHz (HCLK/2). */
	USART1->BRR = (uint16_t)((84000000UL + (115200UL / 2UL)) / 115200UL);
}

static void EarlyUart1_Putc(char c)
{
	while ((USART1->SR & USART_SR_TXE) == 0U)
	{
	}
	USART1->DR = (uint16_t)c;
}

static void EarlyUart1_Puts(const char *s)
{
	while (*s != '\0')
	{
		if (*s == '\n')
		{
			EarlyUart1_Putc('\r');
		}
		EarlyUart1_Putc(*s++);
	}
}

void SystemClock_Config(void);
extern void vStartLEDTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority);
extern void vStartKeyTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority);

/* Task priority policy:
 * Gateway(10) > Alarm(4) > Sensor(3) > Display/Key(2) > LED(1).
 * Keep this order unless there is measured latency starvation.
 */

int main(void)
{
	EarlyUart1_Init();
	EarlyUart1_Puts("[EARLY] enter main\n");

	HAL_Init();
	EarlyUart1_Puts("[EARLY] HAL_Init done\n");
	EarlyUart1_Puts("[EARLY] before SystemClock_Config\n");
	SystemClock_Config();
	EarlyUart1_ReconfigAfterClock();
	EarlyUart1_Puts("[EARLY] SystemClock_Config done\n");
	EarlyUart1_Puts("[EARLY] build_tag=" APP_BUILD_TAG "\n");
	EarlyUart1_Puts("[EARLY] ota early confirm handled in gateway task\n");

	SmartHomeConfig_Init();
	SmartHomeState_Init();
	Log_SetLevel(LOG_DEFAULT_LEVEL);
	vStartStorageTasks(384, 2);
	FaultDiag_InitAtBoot();
	LOG_INFO("MAIN", "system init done");
	LOG_INFO("MAIN", "build_tag=%s", APP_BUILD_TAG);
	LOG_INFO("MAIN", "log level=%s", Log_LevelToString(Log_GetLevel()));

#if SH_USE_GATEWAY_MODE
	vStartGatewayTasks(512, 10);
#endif
#if SH_USE_MQTT_MODE
	extern void vStartMQTTTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority);
	vStartMQTTTasks(1024, 5);
#endif
	vStartSensorTasks(512, 3);
	vStartAlarmTasks(384, 4);
	vStartDisplayTasks(512, 2);
	vStartLEDTasks(128, 1);
	vStartKeyTasks(128, 2);
	Watchdog_Init();
	Watchdog_RegisterTask(WD_TASK_GATEWAY, "gateway", 6000U);
	Watchdog_RegisterTask(WD_TASK_SENSOR, "sensor", 6000U);
	Watchdog_RegisterTask(WD_TASK_ALARM, "alarm", 6000U);
	Watchdog_RegisterTask(WD_TASK_STORAGE, "storage", 12000U);
	Watchdog_RegisterTask(WD_TASK_DISPLAY, "display", 6000U);
	Watchdog_RegisterTask(WD_TASK_KEY, "key", 6000U);
	Watchdog_RegisterTask(WD_TASK_LED, "led", 6000U);
	WatchdogTask_Start(256, 2U);
	vTaskStartScheduler();
	while (1)
	{
	}
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
	(void)xTask;
	EarlyUart1_Puts("[FATAL][RTOS] stack overflow task=");
	EarlyUart1_Puts((pcTaskName != NULL) ? pcTaskName : "unknown");
	EarlyUart1_Puts("\n");
	LOG_ERROR("RTOS", "stack overflow task=%s", (pcTaskName != NULL) ? pcTaskName : "unknown");
	taskDISABLE_INTERRUPTS();
	while (1)
	{
	}
}

void vApplicationMallocFailedHook(void)
{
	LOG_ERROR("RTOS", "malloc failed free_heap=%u", (unsigned int)xPortGetFreeHeapSize());
	taskDISABLE_INTERRUPTS();
	while (1)
	{
	}
}

void SystemClock_Config(void)
{
#if defined(SH_MCU_F407)
	RCC_OscInitTypeDef RCC_OscInitStruct = {0};
	RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

	__HAL_RCC_PWR_CLK_ENABLE();
	__HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

	RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
	RCC_OscInitStruct.HSEState = RCC_HSE_ON;
	RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
	RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
	RCC_OscInitStruct.PLL.PLLM = 8;
	RCC_OscInitStruct.PLL.PLLN = 336;
	RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
	RCC_OscInitStruct.PLL.PLLQ = 7;
	if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
	{
		while (1)
		{
		}
	}

	RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
								 RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
	RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
	RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
	RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
	RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;
	if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
	{
		while (1)
		{
		}
	}
#else
	RCC_OscInitTypeDef RCC_OscInitStruct = {0};
	RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

	RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
	RCC_OscInitStruct.HSEState = RCC_HSE_ON;
	RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
	RCC_OscInitStruct.HSIState = RCC_HSI_ON;
	RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
	RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
	RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
	HAL_RCC_OscConfig(&RCC_OscInitStruct);

	RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
	RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
	RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
	RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
	RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

	HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2);
#endif
}
