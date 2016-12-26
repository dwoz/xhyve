#pragma once

#include <stdint.h>
#include <stdbool.h>

const char *bootrom(void);
void bootrom_init(const char *bootrom_path);
uint64_t bootrom_load(void);
bool bootrom_contains_gpa(uint64_t gpa);
