#include "bootloader_uart.h"

#include "board_pins.h"
#include "stm32f4xx_hal.h"

static UART_HandleTypeDef g_bl_uart;
static DMA_HandleTypeDef g_bl_uart_rx_dma;
static uint32_t g_bl_last_tx_hal_status = (uint32_t)HAL_OK;
static uint32_t g_bl_last_tx_req_len = 0U;
static uint8_t g_bl_uart_rx_dma_buf[256];
static volatile uint8_t g_bl_uart_rx_ring[512];
static volatile uint16_t g_bl_uart_rx_head = 0U;
static volatile uint16_t g_bl_uart_rx_tail = 0U;
static volatile uint16_t g_bl_uart_rx_dma_last_pos = 0U;

#define BL_UART_RX_DMA_INSTANCE       DMA1_Stream1
#define BL_UART_RX_DMA_CHANNEL        DMA_CHANNEL_4
#define BL_UART_RX_DMA_IRQn           DMA1_Stream1_IRQn
#define BL_UART_RX_DMA_BUF_SIZE       ((uint16_t)sizeof(g_bl_uart_rx_dma_buf))
#define BL_UART_RX_RING_SIZE          ((uint16_t)sizeof(g_bl_uart_rx_ring))

static void bl_uart_rx_ring_push(uint8_t byte) {
    uint16_t next = (uint16_t)((g_bl_uart_rx_head + 1U) % BL_UART_RX_RING_SIZE);
    if (next == g_bl_uart_rx_tail) {
        g_bl_uart_rx_tail = (uint16_t)((g_bl_uart_rx_tail + 1U) % BL_UART_RX_RING_SIZE);
    }
    g_bl_uart_rx_ring[g_bl_uart_rx_head] = byte;
    g_bl_uart_rx_head = next;
}

static int bl_uart_rx_ring_pop(uint8_t* out) {
    if (out == 0 || g_bl_uart_rx_head == g_bl_uart_rx_tail) {
        return 0;
    }
    *out = g_bl_uart_rx_ring[g_bl_uart_rx_tail];
    g_bl_uart_rx_tail = (uint16_t)((g_bl_uart_rx_tail + 1U) % BL_UART_RX_RING_SIZE);
    return 1;
}

static void bl_uart_dma_consume_to(uint16_t pos) {
    if (pos > BL_UART_RX_DMA_BUF_SIZE) {
        pos = BL_UART_RX_DMA_BUF_SIZE;
    }
    while (g_bl_uart_rx_dma_last_pos != pos) {
        bl_uart_rx_ring_push(g_bl_uart_rx_dma_buf[g_bl_uart_rx_dma_last_pos]);
        g_bl_uart_rx_dma_last_pos++;
        if (g_bl_uart_rx_dma_last_pos >= BL_UART_RX_DMA_BUF_SIZE) {
            g_bl_uart_rx_dma_last_pos = 0U;
        }
    }
}

static void bl_uart_dma_sync(void) {
    uint16_t pos = (uint16_t)(BL_UART_RX_DMA_BUF_SIZE - (uint16_t)__HAL_DMA_GET_COUNTER(&g_bl_uart_rx_dma));
    if (pos >= BL_UART_RX_DMA_BUF_SIZE) {
        pos = 0U;
    }
    bl_uart_dma_consume_to(pos);
}

static void bl_uart_dma_start(void) {
    g_bl_uart_rx_head = 0U;
    g_bl_uart_rx_tail = 0U;
    g_bl_uart_rx_dma_last_pos = 0U;
    (void)HAL_UART_AbortReceive(&g_bl_uart);
    __HAL_UART_CLEAR_IDLEFLAG(&g_bl_uart);
    (void)HAL_UART_Receive_DMA(&g_bl_uart, g_bl_uart_rx_dma_buf, BL_UART_RX_DMA_BUF_SIZE);
}

void bl_uart_init(void) {
    BOARD_ENABLE_GW_UART_CLK();
    __HAL_RCC_DMA1_CLK_ENABLE();
    if (BOARD_GW_TX_PORT == GPIOA || BOARD_GW_RX_PORT == GPIOA) {
        BOARD_ENABLE_GPIOA_CLK();
    }
    if (BOARD_GW_TX_PORT == GPIOB || BOARD_GW_RX_PORT == GPIOB) {
        BOARD_ENABLE_GPIOB_CLK();
    }

    Board_UART_GPIO_Init_TX(BOARD_GW_TX_PORT, BOARD_GW_TX_PIN, BOARD_GW_UART_AF);
    Board_UART_GPIO_Init_RX(BOARD_GW_RX_PORT, BOARD_GW_RX_PIN, BOARD_GW_UART_AF);

    g_bl_uart.Instance = BOARD_GW_UART_INSTANCE;
    g_bl_uart.Init.BaudRate = BOARD_UART_BAUD_DEFAULT;
    g_bl_uart.Init.WordLength = UART_WORDLENGTH_8B;
    g_bl_uart.Init.StopBits = UART_STOPBITS_1;
    g_bl_uart.Init.Parity = UART_PARITY_NONE;
    g_bl_uart.Init.Mode = UART_MODE_TX_RX;
    g_bl_uart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    g_bl_uart.Init.OverSampling = UART_OVERSAMPLING_16;

    g_bl_uart_rx_dma.Instance = BL_UART_RX_DMA_INSTANCE;
    g_bl_uart_rx_dma.Init.Channel = BL_UART_RX_DMA_CHANNEL;
    g_bl_uart_rx_dma.Init.Direction = DMA_PERIPH_TO_MEMORY;
    g_bl_uart_rx_dma.Init.PeriphInc = DMA_PINC_DISABLE;
    g_bl_uart_rx_dma.Init.MemInc = DMA_MINC_ENABLE;
    g_bl_uart_rx_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    g_bl_uart_rx_dma.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    g_bl_uart_rx_dma.Init.Mode = DMA_CIRCULAR;
    g_bl_uart_rx_dma.Init.Priority = DMA_PRIORITY_HIGH;
    g_bl_uart_rx_dma.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    (void)HAL_DMA_Init(&g_bl_uart_rx_dma);
    __HAL_LINKDMA(&g_bl_uart, hdmarx, g_bl_uart_rx_dma);

    (void)HAL_UART_Init(&g_bl_uart);

    HAL_NVIC_SetPriority(BL_UART_RX_DMA_IRQn, 0U, 0U);
    HAL_NVIC_EnableIRQ(BL_UART_RX_DMA_IRQn);
    HAL_NVIC_SetPriority(BOARD_GW_UART_IRQn, 0U, 1U);
    HAL_NVIC_EnableIRQ(BOARD_GW_UART_IRQn);

    bl_uart_dma_start();
    __HAL_DMA_DISABLE_IT(&g_bl_uart_rx_dma, DMA_IT_HT);
    __HAL_UART_ENABLE_IT(&g_bl_uart, UART_IT_IDLE);
}

int bl_uart_read_byte(uint8_t* out, uint32_t timeout_ms) {
    uint32_t start;
    if (out == 0) {
        return 0;
    }
    start = HAL_GetTick();
    while (1) {
        bl_uart_dma_sync();
        if (bl_uart_rx_ring_pop(out)) {
            return 1;
        }
        if (timeout_ms != HAL_MAX_DELAY) {
            if ((HAL_GetTick() - start) >= timeout_ms) {
                return 0;
            }
        }
    }
}

int bl_uart_write_bytes(const uint8_t* data, size_t len) {
    HAL_StatusTypeDef st;
    uint32_t start_tick;
    USART_TypeDef* inst;
    if (data == 0 || len == 0U) {
        g_bl_last_tx_req_len = (uint32_t)len;
        g_bl_last_tx_hal_status = (uint32_t)HAL_ERROR;
        return 0;
    }
    g_bl_last_tx_req_len = (uint32_t)len;
    st = HAL_UART_Transmit(&g_bl_uart, (uint8_t*)data, (uint16_t)len, 500U);
    if (st == HAL_OK) {
        inst = g_bl_uart.Instance;
        start_tick = HAL_GetTick();
        while ((inst->SR & USART_SR_TC) == 0U) {
            if ((HAL_GetTick() - start_tick) > 500U) {
                st = HAL_TIMEOUT;
                break;
            }
        }
    }
    g_bl_last_tx_hal_status = (uint32_t)st;
    return (st == HAL_OK) ? 1 : 0;
}

uint32_t bl_uart_last_tx_hal_status(void) {
    return g_bl_last_tx_hal_status;
}

uint32_t bl_uart_last_tx_req_len(void) {
    return g_bl_last_tx_req_len;
}

void bl_uart_irq_handler(void) {
    if (__HAL_UART_GET_FLAG(&g_bl_uart, UART_FLAG_IDLE) != RESET) {
        __HAL_UART_CLEAR_IDLEFLAG(&g_bl_uart);
        bl_uart_dma_sync();
    }
    HAL_UART_IRQHandler(&g_bl_uart);
}

void bl_uart_dma_irq_handler(void) {
    HAL_DMA_IRQHandler(&g_bl_uart_rx_dma);
    bl_uart_dma_sync();
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef* huart) {
    if (huart == &g_bl_uart) {
        bl_uart_dma_start();
        __HAL_DMA_DISABLE_IT(&g_bl_uart_rx_dma, DMA_IT_HT);
        __HAL_UART_ENABLE_IT(&g_bl_uart, UART_IT_IDLE);
    }
}
