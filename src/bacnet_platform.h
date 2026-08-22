#ifndef PHP_BACNET_PLATFORM_H
#define PHP_BACNET_PLATFORM_H

#include <stdint.h>

#ifdef PHP_WIN32
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

static inline uint64_t php_bacnet_platform_monotonic_ms(void) {
	return (uint64_t)GetTickCount64();
}

static inline uint64_t php_bacnet_platform_wall_ms(void) {
	FILETIME file_time;
	ULARGE_INTEGER value;
	GetSystemTimeAsFileTime(&file_time);
	value.LowPart = file_time.dwLowDateTime;
	value.HighPart = file_time.dwHighDateTime;
	return (uint64_t)(value.QuadPart / UINT64_C(10000) - UINT64_C(11644473600000));
}
#else
#include <time.h>

static inline uint64_t php_bacnet_platform_monotonic_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * UINT64_C(1000) + (uint64_t)ts.tv_nsec / UINT64_C(1000000);
}

static inline uint64_t php_bacnet_platform_wall_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * UINT64_C(1000) + (uint64_t)ts.tv_nsec / UINT64_C(1000000);
}
#endif

#endif
