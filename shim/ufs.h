#pragma once
#include <stdint.h>
void test_ufs(void);
uint32_t ufs_log_write(const void *buf, uint32_t len);
int ufs_ready(void);
void wdt_set(unsigned unit16);
void wdt_off(void);
void mark(uint32_t code);
