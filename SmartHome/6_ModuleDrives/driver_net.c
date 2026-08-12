#include "driver_net.h"
#include "driver_buffer.h"
#include "board_pins.h"
#include "string.h"
#include "stdio.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

static void HAL_UART2_MspInit(UART_HandleTypeDef *huart);
void NetDataProcess_Callback(uint8_t data);
static UART_HandleTypeDef huart2;
static RingBuffer CMDRetBuffer;
static RingBuffer NetDataBuffer;
static QueueHandle_t xNetUartRxQueue = NULL;
static volatile uint32_t g_net_rx_isr_drop_count = 0U;

static void Driver_Net_PumpRxQueue(void)
{
	uint8_t rx_data = 0;
	if (xNetUartRxQueue == NULL)
	{
		return;
	}
	while (xQueueReceive(xNetUartRxQueue, &rx_data, 0) == pdPASS)
	{
		Driver_Buffer_Write(&CMDRetBuffer, rx_data);
		NetDataProcess_Callback(rx_data);
	}
}

static int Driver_Net_UART_Init(void)
{
	huart2.Instance = BOARD_NET_UART_INSTANCE;
	huart2.Init.BaudRate = BOARD_UART_BAUD_DEFAULT;
	huart2.Init.WordLength = UART_WORDLENGTH_8B;
	huart2.Init.StopBits = UART_STOPBITS_1;
	huart2.Init.Parity = UART_PARITY_NONE;
	huart2.Init.Mode = UART_MODE_TX_RX;
	huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
	huart2.Init.OverSampling = UART_OVERSAMPLING_16;

	HAL_UART2_MspInit(&huart2);

	if (HAL_UART_Init(&huart2) != HAL_OK)
	{
		return -1;
	}

	__HAL_UART_ENABLE_IT(&huart2, UART_IT_RXNE);
	return 0;
}

static void HAL_UART2_MspInit(UART_HandleTypeDef *huart)
{
	if (huart->Instance == BOARD_NET_UART_INSTANCE)
	{
		BOARD_ENABLE_NET_UART_CLK();
		if (BOARD_NET_TX_PORT == GPIOA || BOARD_NET_RX_PORT == GPIOA)
		{
			BOARD_ENABLE_GPIOA_CLK();
		}
		if (BOARD_NET_TX_PORT == GPIOB || BOARD_NET_RX_PORT == GPIOB)
		{
			BOARD_ENABLE_GPIOB_CLK();
		}

		Board_UART_GPIO_Init_TX(BOARD_NET_TX_PORT, BOARD_NET_TX_PIN, BOARD_NET_UART_AF);
		Board_UART_GPIO_Init_RX(BOARD_NET_RX_PORT, BOARD_NET_RX_PIN, BOARD_NET_UART_AF);

		HAL_NVIC_SetPriority(BOARD_NET_UART_IRQn, 1, 0);
		HAL_NVIC_EnableIRQ(BOARD_NET_UART_IRQn);
	}
}

void BOARD_NET_UART_IRQ_HANDLER(void)
{
	uint8_t rx_data = 0;
	if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_RXNE) == SET)
	{
		BaseType_t xHigherPriorityTaskWoken = pdFALSE;
		__HAL_UART_CLEAR_FLAG(&huart2, UART_FLAG_RXNE);
		rx_data = (uint8_t)(huart2.Instance->DR & 0xFFU);
		if (xNetUartRxQueue != NULL)
		{
			if (xQueueSendToBackFromISR(xNetUartRxQueue, &rx_data, &xHigherPriorityTaskWoken) != pdPASS)
			{
				g_net_rx_isr_drop_count++;
			}
		}
		portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
	}
}

uint32_t Driver_Net_GetRxIsrDropCount(void)
{
	return g_net_rx_isr_drop_count;
}

void Driver_Net_ClearRxIsrDropCount(void)
{
	g_net_rx_isr_drop_count = 0U;
}

static int Driver_Net_TransmitCmd(const char *cmd, const char *reply, uint16_t timeout)
{
	uint8_t i = 0;
	char buf[128] = {0};
	snprintf(buf, sizeof(buf), "%s", cmd);
	if (strstr(buf, "\r\n") == NULL)
	{
		strncat(buf, "\r\n", sizeof(buf) - strlen(buf) - 1U);
	}
	Driver_Buffer_Clean(&CMDRetBuffer);
	HAL_UART_Transmit(&huart2, (uint8_t *)buf, strlen(buf), 500);
	memset(buf, 0, sizeof(buf));

	while (timeout != 0)
	{
		Driver_Net_PumpRxQueue();
		if (Driver_Buffer_Read(&CMDRetBuffer, (uint8_t *)&buf[i]) == 0)
		{
			i = (uint8_t)((i + 1) % sizeof(buf));
			if (strstr(buf, reply) != 0)
			{
				return 0;
			}
		}
		else
		{
			timeout--;
			HAL_Delay(1);
		}
	}
	return -1;
}

int Driver_Net_TransmitSocket(const char *socket, int len, int timeout)
{
	uint8_t i = 0;
	char buf[64] = {0};
	char cmd[16] = {0};

	snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d\r\n", len);
	HAL_UART_Transmit(&huart2, (uint8_t *)cmd, strlen(cmd), 500);
	HAL_Delay(100);
	Driver_Buffer_Clean(&CMDRetBuffer);
	HAL_UART_Transmit(&huart2, (uint8_t *)socket, len, 500);

	while (timeout != 0)
	{
		Driver_Net_PumpRxQueue();
		if (Driver_Buffer_Read(&CMDRetBuffer, (uint8_t *)&buf[i]) == 0)
		{
			i = (uint8_t)((i + 1) % sizeof(buf));
			if (strstr(buf, "SEND OK") != 0)
			{
				return 0;
			}
		}
		else
		{
			timeout--;
			HAL_Delay(1);
		}
	}
	return -1;
}

int Driver_Net_RecvSocket(char *buf, int len, int timeout)
{
	static int tmp = 0;
	int read_len;
	int request_len;
	(void)timeout;
	Driver_Net_PumpRxQueue();

	if (buf == NULL || len <= 0)
	{
		tmp = 0;
		return -1;
	}
	if (tmp >= len)
	{
		tmp = 0;
	}
	request_len = len - tmp;
	if (request_len > 255)
	{
		request_len = 255;
	}
	read_len = Driver_Buffer_ReadBytes(&NetDataBuffer, (uint8_t *)&buf[tmp], (uint8_t)request_len);
	if (read_len < 0)
	{
		return 1;
	}
	tmp += read_len;
	if (tmp == len)
	{
		tmp = 0;
		return 0;
	}
	return 1;
}

int Driver_Net_ConnectWiFi(const char *ssid, const char *pwd, int timeout)
{
	char buf[128] = {0};
	snprintf(buf, sizeof(buf), "AT+CWJAP_CUR=\"%s\",\"%s\"", ssid, pwd);
	return Driver_Net_TransmitCmd(buf, "GOT IP\r\n", timeout);
}

int Driver_Net_DisconnectWiFi(void)
{
	return Driver_Net_TransmitCmd("AT+CWQAP", "OK\r\n", 500);
}

int Driver_Net_ConnectTCP(const char *ip, int port, int timeout)
{
	Driver_Net_TransmitCmd("AT+CIPMUX=0", "OK\r\n", 500);
	HAL_Delay(1);
	Driver_Net_TransmitCmd("AT+CIPMODE=1", "OK\r\n", 500);
	HAL_Delay(1);
	char buf[128] = {0};
	snprintf(buf, sizeof(buf), "AT+CIPSTART=\"TCP\",\"%s\",%d", ip, port);
	return Driver_Net_TransmitCmd(buf, "OK\r\n", timeout);
}

int Driver_Net_Disconnect_TCP_UDP(void)
{
	return Driver_Net_TransmitCmd("AT+CIPCLOSE", "OK\r\n", 500);
}

typedef enum AT_STATUS {
	INIT_STATUS,
	LEN_STATUS,
	DATA_STATUS
} AT_STATUS;

static uint8_t g_DataBuff[256] = {0};
void NetDataProcess_Callback(uint8_t data)
{
	uint8_t *buf = g_DataBuff;
	static AT_STATUS g_status = INIT_STATUS;
	static int g_DataBuffIndex = 0;
	static int g_DataLen = 0;
	int i = g_DataBuffIndex;
	int m = 0;

	if (g_DataBuffIndex >= (int)sizeof(g_DataBuff))
	{
		g_status = INIT_STATUS;
		g_DataBuffIndex = 0;
		g_DataLen = 0;
		i = 0;
	}
	buf[i] = data;
	g_DataBuffIndex++;

	switch (g_status)
	{
		case INIT_STATUS:
		{
			if (buf[0] != '+')
			{
				g_DataBuffIndex = 0;
			}
			else if (i == 4)
			{
				if (strncmp((char *)buf, "+IPD,", 5) == 0)
				{
					g_status = LEN_STATUS;
				}
				g_DataBuffIndex = 0;
			}
			break;
		}

		case LEN_STATUS:
		{
			if (buf[i] == ':')
			{
				g_DataLen = 0;
				for (m = 0; m < i; m++)
				{
					if (buf[m] < '0' || buf[m] > '9')
					{
						g_status = INIT_STATUS;
						g_DataBuffIndex = 0;
						g_DataLen = 0;
						break;
					}
					g_DataLen = g_DataLen * 10 + buf[m] - '0';
				}
				if (g_status == LEN_STATUS)
				{
					if (g_DataLen <= 0 || g_DataLen > (int)sizeof(g_DataBuff))
					{
						g_status = INIT_STATUS;
						g_DataLen = 0;
					}
					else
					{
						g_status = DATA_STATUS;
					}
					g_DataBuffIndex = 0;
				}
			}
			else if (i >= 9)
			{
				g_status = INIT_STATUS;
				g_DataBuffIndex = 0;
			}
			break;
		}

		case DATA_STATUS:
		{
			if (g_DataBuffIndex == g_DataLen)
			{
				if (g_DataLen > 0 && g_DataLen <= (int)sizeof(g_DataBuff))
				{
					Driver_Buffer_WriteBytes(&NetDataBuffer, buf, (uint16_t)g_DataLen);
				}
				g_status = INIT_STATUS;
				g_DataBuffIndex = 0;
				g_DataLen = 0;
			}
			break;
		}
		default:
			break;
	}
}

int Driver_Net_Init(void)
{
	if (Driver_Net_UART_Init() != 0)
	{
		return -1;
	}
	if (xNetUartRxQueue == NULL)
	{
		xNetUartRxQueue = xQueueCreate(256, sizeof(uint8_t));
		if (xNetUartRxQueue == NULL)
		{
			return -1;
		}
	}

	if (Driver_Buffer_Init(&CMDRetBuffer, 128) != 0)
	{
		return -1;
	}
	if (Driver_Buffer_Init(&NetDataBuffer, 1024) != 0)
	{
		return -1;
	}

	Driver_Net_TransmitCmd("AT+RST", "OK\r\n", 10000);
	HAL_Delay(500);
	Driver_Net_TransmitCmd("AT+CWMODE_CUR=1", "OK\r\n", 500);

	return 0;
}
