#include "log.h"

static volatile uint8_t g_log_level = (uint8_t)LOG_DEFAULT_LEVEL;

void Log_SetLevel(LogLevel level)
{
	if (level > LOG_LEVEL_INFO)
	{
		level = LOG_LEVEL_INFO;
	}
	g_log_level = (uint8_t)level;
}

LogLevel Log_GetLevel(void)
{
	return (LogLevel)g_log_level;
}

const char *Log_LevelToString(LogLevel level)
{
	switch (level)
	{
		case LOG_LEVEL_ERROR: return "error";
		case LOG_LEVEL_WARN: return "warn";
		case LOG_LEVEL_INFO: return "info";
		default: return "unknown";
	}
}

uint8_t Log_ShouldPrint(LogLevel level)
{
	return ((uint8_t)level <= g_log_level) ? 1U : 0U;
}
