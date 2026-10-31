#pragma once
#include <stdbool.h>
#include <stdint.h>
void hw_bridge_start(void);
int32_t hw_bridge_fill_pcm(uint8_t *data, int32_t len);
void hw_bridge_set_connected(bool connected);
bool hw_bridge_take_pair_request(void);
