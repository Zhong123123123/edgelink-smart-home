#ifndef __LOG_H
#define __LOG_H

#include <stdio.h>
#include <stdint.h>

typedef enum
{
	LOG_LEVEL_ERROR = 0,
	LOG_LEVEL_WARN = 1,
	LOG_LEVEL_INFO = 2,
} LogLevel;

#ifndef LOG_DEFAULT_LEVEL
#ifdef LOG_LEVEL
#define LOG_DEFAULT_LEVEL LOG_LEVEL
#else
#define LOG_DEFAULT_LEVEL LOG_LEVEL_INFO
#endif
#endif

#define LOG_PRINT(level_str, module, fmt, ...) \
	do { \
		printf("[%s][%s] " fmt "\r\n", level_str, module, ##__VA_ARGS__); \
	} while (0)

void Log_SetLevel(LogLevel level);
LogLevel Log_GetLevel(void);
const char *Log_LevelToString(LogLevel level);
uint8_t Log_ShouldPrint(LogLevel level);

#define LOG_INFO(module, fmt, ...) \
	do { if (Log_ShouldPrint(LOG_LEVEL_INFO)) LOG_PRINT("INFO", module, fmt, ##__VA_ARGS__); } while (0)

#define LOG_WARN(module, fmt, ...) \
	do { if (Log_ShouldPrint(LOG_LEVEL_WARN)) LOG_PRINT("WARN", module, fmt, ##__VA_ARGS__); } while (0)

#define LOG_ERROR(module, fmt, ...) \
	do { if (Log_ShouldPrint(LOG_LEVEL_ERROR)) LOG_PRINT("ERROR", module, fmt, ##__VA_ARGS__); } while (0)

#endif
