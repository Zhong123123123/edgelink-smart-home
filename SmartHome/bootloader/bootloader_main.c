#include "bootloader_config.h"
#include "bootloader_metadata.h"
#include "bootloader_ota.h"
#include "bootloader_uart.h"

#include <stdint.h>
#include "stm32f4xx_hal.h"
#include "stm32f4xx.h"

#if BL_LOG_UART_ENABLE
static void bl_uart_log_init(void) {
    /* PA9 -> USART1_TX (AF7), 115200 8N1 */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

    GPIOA->MODER &= ~(3UL << (9U * 2U));
    GPIOA->MODER |=  (2UL << (9U * 2U));          /* AF mode */
    GPIOA->OSPEEDR |= (3UL << (9U * 2U));         /* High speed */
    GPIOA->AFR[1] &= ~(0xFUL << ((9U - 8U) * 4U));
    GPIOA->AFR[1] |=  (7UL  << ((9U - 8U) * 4U)); /* AF7 */

    USART1->CR1 = 0U;
    USART1->CR2 = 0U;
    USART1->CR3 = 0U;
    USART1->BRR = (uint16_t)((BL_LOG_APB2_CLK_HZ + (BL_LOG_BAUDRATE / 2U)) / BL_LOG_BAUDRATE);
    USART1->CR1 = USART_CR1_TE | USART_CR1_UE;
}

static void bl_uart_putc(char c) {
    while ((USART1->SR & USART_SR_TXE) == 0U) { }
    USART1->DR = (uint16_t)c;
}

static void bl_uart_puts(const char* s) {
    while (*s != '\0') {
        if (*s == '\n') bl_uart_putc('\r');
        bl_uart_putc(*s++);
    }
}

static void bl_uart_put_hex32(uint32_t v) {
    static const char hex[] = "0123456789ABCDEF";
    bl_uart_puts("0x");
    for (int i = 7; i >= 0; --i) {
        bl_uart_putc(hex[(v >> (i * 4)) & 0xFU]);
    }
}

static void bl_uart_put_dec(uint32_t v) {
    char buf[11];
    int i = 0;
    if (v == 0U) {
        bl_uart_putc('0');
        return;
    }
    while (v > 0U && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (v % 10U));
        v /= 10U;
    }
    while (i > 0) {
        bl_uart_putc(buf[--i]);
    }
}
#else
static void bl_uart_log_init(void) {}
static void bl_uart_puts(const char* s) { (void)s; }
static void bl_uart_put_hex32(uint32_t v) { (void)v; }
static void bl_uart_put_dec(uint32_t v) { (void)v; }
#endif

/* Boot diagnostics: read with debugger to confirm power-on decision path. */
volatile uint32_t g_bl_dbg_stage;
volatile uint32_t g_bl_dbg_rcc_csr;
volatile uint32_t g_bl_dbg_target;
volatile uint32_t g_bl_dbg_app_sp;
volatile uint32_t g_bl_dbg_app_pc;
volatile uint32_t g_bl_dbg_meta_magic;
volatile uint32_t g_bl_dbg_meta_active_slot;
volatile uint32_t g_bl_dbg_meta_pending_slot;
volatile uint32_t g_bl_dbg_meta_app_a_state;
volatile uint32_t g_bl_dbg_meta_app_b_state;

static void bl_log_reset_flags(uint32_t csr) {
    bl_uart_puts("[BL] reset flags:");
    if ((csr & RCC_CSR_LPWRRSTF) != 0U) bl_uart_puts(" LPWR");
    if ((csr & RCC_CSR_WWDGRSTF) != 0U) bl_uart_puts(" WWDG");
    if ((csr & RCC_CSR_WDGRSTF) != 0U) bl_uart_puts(" IWDG");
    if ((csr & RCC_CSR_SFTRSTF) != 0U) bl_uart_puts(" SFT");
    if ((csr & RCC_CSR_PORRSTF) != 0U) bl_uart_puts(" POR");
    if ((csr & RCC_CSR_PINRSTF) != 0U) bl_uart_puts(" PIN");
    if ((csr & RCC_CSR_BORRSTF) != 0U) bl_uart_puts(" BOR");
    bl_uart_puts("\n");
}

static int bl_is_vector_valid(uint32_t app_base) {
    uint32_t initial_sp = *(volatile uint32_t*)app_base;
    uint32_t reset_handler = *(volatile uint32_t*)(app_base + 4U);

    bl_uart_puts("[BL] check vector base=");
    bl_uart_put_hex32(app_base);
    bl_uart_puts(" sp=");
    bl_uart_put_hex32(initial_sp);
    bl_uart_puts(" pc=");
    bl_uart_put_hex32(reset_handler);
    bl_uart_puts("\n");

    if ((initial_sp & 0x2FFE0000UL) != 0x20000000UL) return 0;
    if (reset_handler < BL_FLASH_BASE || reset_handler >= (BL_FLASH_BASE + 0x100000UL)) return 0;
    return 1;
}

__attribute__((noreturn, naked)) static void bl_jump_to_app_entry(uint32_t app_sp, uint32_t app_pc) {
    __asm volatile (
        "msr msp, r0      \n"
        "bx  r1           \n"
    );
}

static void bl_jump_to_app(uint32_t app_base) {
    uint32_t app_sp = *(volatile uint32_t*)app_base;
    uint32_t app_pc = *(volatile uint32_t*)(app_base + 4U);

    g_bl_dbg_stage = 4U;
    g_bl_dbg_app_sp = app_sp;
    g_bl_dbg_app_pc = app_pc;
    bl_uart_puts("[BL] jump base=");
    bl_uart_put_hex32(app_base);
    bl_uart_puts(" sp=");
    bl_uart_put_hex32(app_sp);
    bl_uart_puts(" pc=");
    bl_uart_put_hex32(app_pc);
    bl_uart_puts("\n");

    __disable_irq();
    /* Fully quiesce exception state before handing off to app vector table. */
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL = 0U;
    for (uint32_t i = 0; i < 8U; ++i) {
        NVIC->ICER[i] = 0xFFFFFFFFUL;
        NVIC->ICPR[i] = 0xFFFFFFFFUL;
    }
    MPU->CTRL = 0U;
    SCB->SHCSR &= ~SCB_SHCSR_MEMFAULTENA_Msk;
    SCB->ICSR = SCB_ICSR_PENDSVCLR_Msk | SCB_ICSR_PENDSTCLR_Msk;
    SCB->CFSR = SCB->CFSR;
    SCB->HFSR = SCB->HFSR;
    SCB->DFSR = SCB->DFSR;
    SCB->MMFAR = 0U;
    SCB->BFAR = 0U;

    SCB->VTOR = app_base;
    __DSB();
    __ISB();
    __set_CONTROL(0U);
    __set_PSP(0U);
    __DSB();
    __ISB();
    bl_jump_to_app_entry(app_sp, app_pc);
}

static uint32_t bl_select_slot(const ota_metadata_t* meta) {
    if (meta->pending_slot == 1U && meta->app_b_state == IMG_PENDING) return BL_APP_B_BASE;
    if (meta->pending_slot == 0U && meta->app_a_state == IMG_PENDING) return BL_APP_A_BASE;
    if (meta->active_slot == 1U) return BL_APP_B_BASE;
    return BL_APP_A_BASE;
}

static void bl_backup_domain_enable(void) {
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    PWR->CR |= PWR_CR_DBP;
    RCC->BDCR |= RCC_BDCR_RTCEN;
}

static int bl_consume_force_bootloader_flag(void) {
    uint32_t flag;
    bl_backup_domain_enable();
    flag = RTC->BKP0R;
    if (flag == 0xB00710ADUL) {
        RTC->BKP0R = 0U;
        return 1;
    }
    return 0;
}

static uint32_t bl_slot_base(uint8_t slot) {
    return (slot == BL_SLOT_B) ? BL_APP_B_BASE : BL_APP_A_BASE;
}

static uint32_t bl_slot_size(uint8_t slot) {
    (void)slot;
    return BL_APP_A_SIZE;
}

static uint32_t bl_slot_crc(const ota_metadata_t* meta, uint8_t slot) {
    return (slot == BL_SLOT_B) ? meta->app_b_crc32 : meta->app_a_crc32;
}

static uint32_t bl_slot_image_size(const ota_metadata_t* meta, uint8_t slot) {
    return (slot == BL_SLOT_B) ? meta->app_b_size : meta->app_a_size;
}

static void bl_set_slot_state(ota_metadata_t* meta, uint8_t slot, uint32_t state) {
    if (slot == BL_SLOT_B) meta->app_b_state = state;
    else meta->app_a_state = state;
}

static int bl_slot_image_valid(const ota_metadata_t* meta, uint8_t slot) {
    uint32_t base = bl_slot_base(slot);
    uint32_t size = bl_slot_image_size(meta, slot);
    uint32_t crc = bl_slot_crc(meta, slot);
    uint32_t calc_crc = 0U;
    int vector_ok = 0;
    int size_ok = (size != 0U && size <= bl_slot_size(slot)) ? 1 : 0;

    bl_uart_puts("[BL] slot check slot=");
    bl_uart_put_dec((uint32_t)slot);
    bl_uart_puts(" base=");
    bl_uart_put_hex32(base);
    bl_uart_puts(" size=");
    bl_uart_put_dec(size);
    bl_uart_puts(" limit=");
    bl_uart_put_dec(bl_slot_size(slot));
    bl_uart_puts("\n");

    if (!size_ok) {
        bl_uart_puts("[BL] slot check fail: size invalid\n");
        return 0;
    }

    vector_ok = bl_is_vector_valid(base);
    if (!vector_ok) {
        bl_uart_puts("[BL] slot check fail: vector invalid\n");
        return 0;
    }

    calc_crc = bl_crc32((const uint8_t*)base, size);
    bl_uart_puts("[BL] slot check crc meta=");
    bl_uart_put_hex32(crc);
    bl_uart_puts(" calc=");
    bl_uart_put_hex32(calc_crc);
    bl_uart_puts("\n");
    if (calc_crc != crc) {
        bl_uart_puts("[BL] slot check fail: crc mismatch\n");
        return 0;
    }
    bl_uart_puts("[BL] slot check ok\n");
    return 1;
}

int main(void) {
    ota_metadata_t meta;
    uint32_t target;

    HAL_Init();

    g_bl_dbg_stage = 1U;
    g_bl_dbg_rcc_csr = RCC->CSR;
    bl_uart_log_init();
    bl_uart_puts("\n[BL] boot start\n");
    bl_uart_puts("[BL] RCC->CSR=");
    bl_uart_put_hex32(g_bl_dbg_rcc_csr);
    bl_uart_puts("\n");
    bl_log_reset_flags(g_bl_dbg_rcc_csr);

    if (bl_metadata_any_valid()) {
        bl_metadata_load_or_default(&meta);
        bl_uart_puts("[BL] metadata valid\n");
    } else {
        bl_metadata_default(&meta);
        bl_uart_puts("[BL] metadata invalid, use default\n");
        bl_uart_puts("[BL] metadata invalid reason=");
        bl_uart_put_hex32(g_bl_meta_validate_reason);
        bl_uart_puts("\n");
        if (bl_metadata_commit(&meta)) {
            bl_uart_puts("[BL] heal metadata: write ok\n");
        } else {
            bl_uart_puts("[BL] heal metadata: write fail\n");
        }
    }

    g_bl_dbg_stage = 2U;
    g_bl_dbg_meta_magic = meta.magic;
    g_bl_dbg_meta_active_slot = meta.active_slot;
    g_bl_dbg_meta_pending_slot = meta.pending_slot;
    g_bl_dbg_meta_app_a_state = meta.app_a_state;
    g_bl_dbg_meta_app_b_state = meta.app_b_state;
    bl_uart_puts("[BL] meta magic=");
    bl_uart_put_hex32(meta.magic);
    bl_uart_puts(" active=");
    bl_uart_put_hex32(meta.active_slot);
    bl_uart_puts(" pending=");
    bl_uart_put_hex32(meta.pending_slot);
    bl_uart_puts(" a_state=");
    bl_uart_put_hex32(meta.app_a_state);
    bl_uart_puts(" b_state=");
    bl_uart_put_hex32(meta.app_b_state);
    bl_uart_puts("\n");

#if (BL_DEBUG_FORCE_BOOT_SLOT == 0U) || (BL_DEBUG_FORCE_BOOT_SLOT == 1U)
    bl_uart_puts("[BL] DEBUG force slot=");
    bl_uart_put_hex32(BL_DEBUG_FORCE_BOOT_SLOT);
    bl_uart_puts("\n");
    target = bl_slot_base((uint8_t)BL_DEBUG_FORCE_BOOT_SLOT);
    if (bl_is_vector_valid(target)) {
        bl_uart_puts("[BL] DEBUG force jump\n");
        bl_jump_to_app(target);
    } else {
        bl_uart_puts("[BL] DEBUG force slot vector invalid, continue normal flow\n");
    }
#endif

    if (bl_consume_force_bootloader_flag()) {
        bl_uart_puts("[BOOT] ota request magic detected\n");
        bl_uart_init();
        bl_uart_puts("[BOOT] send boot hello\n");
        bl_uart_puts("[BOOT] wait ota command\n");
        (void)bl_ota_loop(&meta, BL_OTA_IDLE_TIMEOUT_MS);
        bl_uart_puts("[BOOT] ota timeout, jump current app\n");
    }

    target = bl_select_slot(&meta);
    g_bl_dbg_target = target;
    g_bl_dbg_stage = 3U;
    bl_uart_puts("[BL] target=");
    bl_uart_put_hex32(target);
    bl_uart_puts("\n");

    if (meta.pending_slot == BL_SLOT_A || meta.pending_slot == BL_SLOT_B) {
        if (!bl_slot_image_valid(&meta, meta.pending_slot)) {
            bl_uart_puts("[BL] pending slot image invalid (size/crc/vector), rollback pending\n");
            bl_set_slot_state(&meta, meta.pending_slot, IMG_ROLLBACK);
            meta.pending_slot = BL_SLOT_NONE;
            meta.rollback_count += 1U;
            meta.boot_attempts = 0U;
            (void)bl_metadata_commit(&meta);
        } else if (meta.boot_attempts >= BL_MAX_BOOT_ATTEMPTS) {
            bl_set_slot_state(&meta, meta.pending_slot, IMG_ROLLBACK);
            meta.pending_slot = BL_SLOT_NONE;
            meta.rollback_count += 1U;
            meta.boot_attempts = 0U;
            (void)bl_metadata_commit(&meta);
        } else {
            meta.boot_attempts += 1U;
            (void)bl_metadata_commit(&meta);
            bl_jump_to_app(bl_slot_base(meta.pending_slot));
        }
    }

    if (meta.active_slot == BL_SLOT_A || meta.active_slot == BL_SLOT_B) {
        uint32_t active_base = bl_slot_base(meta.active_slot);
#if BL_DEV_BYPASS_CRC
        bl_uart_puts("[BL] DEV bypass crc enabled for active slot\n");
        if (bl_is_vector_valid(active_base)) {
            bl_jump_to_app(active_base);
        } else {
            bl_uart_puts("[BL] active slot vector invalid\n");
        }
#else
        if (bl_slot_image_valid(&meta, meta.active_slot)) {
            bl_jump_to_app(active_base);
        } else {
            bl_uart_puts("[BL] active slot image invalid (size/crc/vector)\n");
        }
#endif
    }

    if (meta.confirmed_slot == BL_SLOT_A || meta.confirmed_slot == BL_SLOT_B) {
        uint32_t confirmed_base = bl_slot_base(meta.confirmed_slot);
#if BL_DEV_BYPASS_CRC
        bl_uart_puts("[BL] DEV bypass crc enabled for confirmed slot\n");
        if (bl_is_vector_valid(confirmed_base)) {
            bl_jump_to_app(confirmed_base);
        } else {
            bl_uart_puts("[BL] confirmed slot vector invalid\n");
        }
#else
        if (bl_slot_image_valid(&meta, meta.confirmed_slot)) {
            bl_jump_to_app(confirmed_base);
        } else {
            bl_uart_puts("[BL] confirmed slot image invalid (size/crc/vector)\n");
        }
#endif
    }

    bl_uart_puts("[BL] no bootable slot, stay in bootloader\n");

    g_bl_dbg_stage = 0xE0U;
    bl_uart_puts("[BL] enter ota loop on USART3\n");
    bl_uart_init();
    (void)bl_ota_loop(&meta, 0U);
    while (1) { }
}
