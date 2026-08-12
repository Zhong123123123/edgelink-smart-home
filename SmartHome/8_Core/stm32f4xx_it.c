#include "stm32f4xx_it.h"
#include "fault_diag.h"

void NMI_Handler(void)
{
}

__attribute__((naked)) void HardFault_Handler(void)
{
	__asm volatile(
		"tst lr, #4                \n"
		"ite eq                    \n"
		"mrseq r0, msp             \n"
		"mrsne r0, psp             \n"
		"mov r1, lr                \n"
		"b FaultDiag_HardFaultCapture \n");
}

void MemManage_Handler(void)
{
	while (1)
	{
	}
}

void BusFault_Handler(void)
{
	while (1)
	{
	}
}

void UsageFault_Handler(void)
{
	while (1)
	{
	}
}

void DebugMon_Handler(void)
{
}
