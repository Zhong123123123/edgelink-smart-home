#include "main.h"
extern void KeyShakeProcess_Callback(void);
static TIM_HandleTypeDef htim2;

HAL_StatusTypeDef HAL_InitTick(uint32_t TickPriority)
{
	uint32_t uwTimclock = 0;
	uint32_t uwPrescalerValue = 0;
	
	__HAL_RCC_TIM2_CLK_ENABLE();
	
	HAL_NVIC_SetPriority(TIM2_IRQn, TickPriority, 0);
	HAL_NVIC_EnableIRQ(TIM2_IRQn);
	
	uwTimclock = HAL_RCC_GetPCLK1Freq() * 2;
	uwPrescalerValue = uwTimclock / 1000000;
	
	htim2.Instance = TIM2;
	htim2.Init.Prescaler = uwPrescalerValue - 1;
	htim2.Init.Period = 1000 - 1;
	htim2.Init.ClockDivision = 0;
	htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
	if(HAL_TIM_Base_Init(&htim2) == HAL_OK)
	{
		return HAL_TIM_Base_Start_IT(&htim2);
	}
	return HAL_ERROR;
}

void HAL_SuspendTick(void)
{
	__HAL_TIM_DISABLE_IT(&htim2, TIM_IT_UPDATE);
}

void HAL_ResumeTick(void)
{
	__HAL_TIM_ENABLE_IT(&htim2, TIM_IT_UPDATE);
}

void TIM2_IRQHandler(void)
{
	HAL_TIM_IRQHandler(&htim2);
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
	if (htim != NULL && htim->Instance == TIM2)
	{
		HAL_IncTick();
		KeyShakeProcess_Callback();
	}
}
