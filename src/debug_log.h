#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#ifndef NES_ENABLE_LOGS
#define NES_ENABLE_LOGS 0
#endif

#if NES_ENABLE_LOGS
#include <stdio.h>
#define NES_LOG(...) printf(__VA_ARGS__)
#else
#define NES_LOG(...) do { } while (0)
#endif

#endif // DEBUG_LOG_H
