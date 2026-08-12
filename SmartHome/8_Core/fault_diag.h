#ifndef __FAULT_DIAG_H
#define __FAULT_DIAG_H

#include <stdint.h>

typedef struct
{
	uint32_t magic;
	uint32_t stacked_r0;
	uint32_t stacked_r1;
	uint32_t stacked_r2;
	uint32_t stacked_r3;
	uint32_t stacked_r12;
	uint32_t stacked_lr;
	uint32_t stacked_pc;
	uint32_t stacked_xpsr;
	uint32_t exc_lr;
	uint32_t cfsr;
	uint32_t hfsr;
	uint32_t dfsr;
	uint32_t afsr;
	uint32_t mmfar;
	uint32_t bfar;
	uint32_t tick;
	uint32_t crc32;
} FaultSnapshot;

void FaultDiag_InitAtBoot(void);
void FaultDiag_HardFaultCapture(uint32_t *stack, uint32_t exc_lr);
void FaultDiag_TestInjectOnce(void);

#endif
