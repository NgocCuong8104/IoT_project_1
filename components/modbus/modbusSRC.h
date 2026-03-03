#pragma once
#include <stdint.h>

uint16_t checkModbusCRC(const uint8_t *buf, uint16_t len);
