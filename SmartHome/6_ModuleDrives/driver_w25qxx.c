#include "driver_w25qxx.h"

#include "board_pins.h"

#define W25QXX_CMD_RDID            0x9FU
#define W25QXX_CMD_RDSR1           0x05U
#define W25QXX_CMD_WREN            0x06U
#define W25QXX_CMD_READ            0x03U
#define W25QXX_CMD_PP              0x02U
#define W25QXX_CMD_SE              0x20U

#define W25QXX_PAGE_SIZE           256U
#define W25QXX_SECTOR_SIZE         4096U

static SPI_HandleTypeDef g_w25qxx_spi;
static uint8_t g_w25qxx_ready = 0U;
static uint32_t g_w25qxx_size_bytes = 0U;

static uint8_t w25qxx_read_sr1(void);

static void w25qxx_cs_low(void)
{
	HAL_GPIO_WritePin(BOARD_SPI_FLASH_CS_PORT, BOARD_SPI_FLASH_CS_PIN, GPIO_PIN_RESET);
}

static void w25qxx_cs_high(void)
{
	HAL_GPIO_WritePin(BOARD_SPI_FLASH_CS_PORT, BOARD_SPI_FLASH_CS_PIN, GPIO_PIN_SET);
}

static uint8_t w25qxx_xfer_u8(uint8_t data)
{
	uint8_t rx = 0xFFU;
	(void)HAL_SPI_TransmitReceive(&g_w25qxx_spi, &data, &rx, 1U, 100U);
	return rx;
}

static bool w25qxx_write_enable(void)
{
	uint8_t cmd = W25QXX_CMD_WREN;
	uint32_t start;
	w25qxx_cs_low();
	if (HAL_SPI_Transmit(&g_w25qxx_spi, &cmd, 1U, 100U) != HAL_OK)
	{
		w25qxx_cs_high();
		return false;
	}
	w25qxx_cs_high();

	/* WEL(SR1 bit1) must be set before PP/SE command can take effect. */
	start = HAL_GetTick();
	while ((w25qxx_read_sr1() & 0x02U) == 0U)
	{
		if ((HAL_GetTick() - start) > 10U)
		{
			return false;
		}
	}
	return true;
}

static uint8_t w25qxx_read_sr1(void)
{
	uint8_t cmd = W25QXX_CMD_RDSR1;
	uint8_t sr = 0xFFU;
	w25qxx_cs_low();
	(void)HAL_SPI_Transmit(&g_w25qxx_spi, &cmd, 1U, 100U);
	sr = w25qxx_xfer_u8(0xFFU);
	w25qxx_cs_high();
	return sr;
}

bool W25QXX_WaitBusy(uint32_t timeout_ms)
{
	uint32_t start = HAL_GetTick();
	while ((w25qxx_read_sr1() & 0x01U) != 0U)
	{
		if ((HAL_GetTick() - start) > timeout_ms)
		{
			return false;
		}
	}
	return true;
}

static bool w25qxx_addr_valid(uint32_t addr, uint32_t len)
{
	if (g_w25qxx_size_bytes == 0U)
	{
		return false;
	}
	if (len == 0U)
	{
		return true;
	}
	if (addr >= g_w25qxx_size_bytes)
	{
		return false;
	}
	if (len > (g_w25qxx_size_bytes - addr))
	{
		return false;
	}
	return true;
}

uint32_t W25QXX_ReadJEDECID(void)
{
	uint8_t cmd = W25QXX_CMD_RDID;
	uint8_t id0;
	uint8_t id1;
	uint8_t id2;

	w25qxx_cs_low();
	if (HAL_SPI_Transmit(&g_w25qxx_spi, &cmd, 1U, 100U) != HAL_OK)
	{
		w25qxx_cs_high();
		return 0U;
	}
	id0 = w25qxx_xfer_u8(0xFFU);
	id1 = w25qxx_xfer_u8(0xFFU);
	id2 = w25qxx_xfer_u8(0xFFU);
	w25qxx_cs_high();

	return ((uint32_t)id0 << 16U) | ((uint32_t)id1 << 8U) | (uint32_t)id2;
}

uint32_t W25QXX_GetSizeBytes(void)
{
	return g_w25qxx_size_bytes;
}

bool W25QXX_Init(void)
{
#if defined(SH_MCU_F407)
	GPIO_InitTypeDef gpio = {0};
	uint32_t jedec;
	uint8_t cap;

	BOARD_ENABLE_SPI1_CLK();
	BOARD_ENABLE_GPIOB_CLK();

	gpio.Pin = BOARD_SPI_FLASH_SCK_PIN | BOARD_SPI_FLASH_MISO_PIN | BOARD_SPI_FLASH_MOSI_PIN;
	gpio.Mode = GPIO_MODE_AF_PP;
	gpio.Pull = GPIO_NOPULL;
	gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
	gpio.Alternate = GPIO_AF5_SPI1;
	HAL_GPIO_Init(BOARD_SPI_FLASH_SCK_PORT, &gpio);

	gpio.Pin = BOARD_SPI_FLASH_CS_PIN;
	gpio.Mode = GPIO_MODE_OUTPUT_PP;
	gpio.Pull = GPIO_PULLUP;
	gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
	gpio.Alternate = 0U;
	HAL_GPIO_Init(BOARD_SPI_FLASH_CS_PORT, &gpio);
	w25qxx_cs_high();

	g_w25qxx_spi.Instance = SPI1;
	g_w25qxx_spi.Init.Mode = SPI_MODE_MASTER;
	g_w25qxx_spi.Init.Direction = SPI_DIRECTION_2LINES;
	g_w25qxx_spi.Init.DataSize = SPI_DATASIZE_8BIT;
	g_w25qxx_spi.Init.CLKPolarity = SPI_POLARITY_LOW;
	g_w25qxx_spi.Init.CLKPhase = SPI_PHASE_1EDGE;
	g_w25qxx_spi.Init.NSS = SPI_NSS_SOFT;
	g_w25qxx_spi.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
	g_w25qxx_spi.Init.FirstBit = SPI_FIRSTBIT_MSB;
	g_w25qxx_spi.Init.TIMode = SPI_TIMODE_DISABLE;
	g_w25qxx_spi.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
	g_w25qxx_spi.Init.CRCPolynomial = 7U;
	if (HAL_SPI_Init(&g_w25qxx_spi) != HAL_OK)
	{
		return false;
	}

	jedec = W25QXX_ReadJEDECID();
	cap = (uint8_t)(jedec & 0xFFU);
	if (cap >= 0x10U && cap <= 0x20U)
	{
		g_w25qxx_size_bytes = (uint32_t)1UL << cap;
	}
	else
	{
		g_w25qxx_size_bytes = 0U;
	}

	g_w25qxx_ready = 1U;
	return (g_w25qxx_size_bytes > 0U);
#else
	g_w25qxx_ready = 0U;
	g_w25qxx_size_bytes = 0U;
	return false;
#endif
}

bool W25QXX_Read(uint32_t addr, uint8_t *buf, uint32_t len)
{
	uint8_t cmd[4];
	if (g_w25qxx_ready == 0U || buf == 0)
	{
		return false;
	}
	if (!w25qxx_addr_valid(addr, len))
	{
		return false;
	}
	if (len == 0U)
	{
		return true;
	}

	cmd[0] = W25QXX_CMD_READ;
	cmd[1] = (uint8_t)((addr >> 16U) & 0xFFU);
	cmd[2] = (uint8_t)((addr >> 8U) & 0xFFU);
	cmd[3] = (uint8_t)(addr & 0xFFU);

	w25qxx_cs_low();
	if (HAL_SPI_Transmit(&g_w25qxx_spi, cmd, sizeof(cmd), 100U) != HAL_OK)
	{
		w25qxx_cs_high();
		return false;
	}
	if (HAL_SPI_Receive(&g_w25qxx_spi, buf, len, 1000U) != HAL_OK)
	{
		w25qxx_cs_high();
		return false;
	}
	w25qxx_cs_high();
	return true;
}

bool W25QXX_PageProgram(uint32_t addr, const uint8_t *buf, uint32_t len)
{
	uint8_t cmd[4];
	if (g_w25qxx_ready == 0U || buf == 0)
	{
		return false;
	}
	if (len == 0U || len > W25QXX_PAGE_SIZE)
	{
		return false;
	}
	if (((addr & (W25QXX_PAGE_SIZE - 1U)) + len) > W25QXX_PAGE_SIZE)
	{
		return false;
	}
	if (!w25qxx_addr_valid(addr, len))
	{
		return false;
	}
	if (!w25qxx_write_enable())
	{
		return false;
	}

	cmd[0] = W25QXX_CMD_PP;
	cmd[1] = (uint8_t)((addr >> 16U) & 0xFFU);
	cmd[2] = (uint8_t)((addr >> 8U) & 0xFFU);
	cmd[3] = (uint8_t)(addr & 0xFFU);

	w25qxx_cs_low();
	if (HAL_SPI_Transmit(&g_w25qxx_spi, cmd, sizeof(cmd), 100U) != HAL_OK)
	{
		w25qxx_cs_high();
		return false;
	}
	if (HAL_SPI_Transmit(&g_w25qxx_spi, (uint8_t *)buf, len, 1000U) != HAL_OK)
	{
		w25qxx_cs_high();
		return false;
	}
	w25qxx_cs_high();
	return W25QXX_WaitBusy(200U);
}

bool W25QXX_SectorErase(uint32_t addr)
{
	uint8_t cmd[4];
	uint32_t base = addr & ~(W25QXX_SECTOR_SIZE - 1U);
	if (g_w25qxx_ready == 0U)
	{
		return false;
	}
	if (!w25qxx_addr_valid(base, 1U))
	{
		return false;
	}
	if (!w25qxx_write_enable())
	{
		return false;
	}

	cmd[0] = W25QXX_CMD_SE;
	cmd[1] = (uint8_t)((base >> 16U) & 0xFFU);
	cmd[2] = (uint8_t)((base >> 8U) & 0xFFU);
	cmd[3] = (uint8_t)(base & 0xFFU);

	w25qxx_cs_low();
	if (HAL_SPI_Transmit(&g_w25qxx_spi, cmd, sizeof(cmd), 100U) != HAL_OK)
	{
		w25qxx_cs_high();
		return false;
	}
	w25qxx_cs_high();
	return W25QXX_WaitBusy(3000U);
}
