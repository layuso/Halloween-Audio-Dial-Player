#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint8_t mode;           /* 0:10, 1:20, 2:30, 3:40, 4: random 10..40 */
    bool enabled;
    bool playing;
    uint64_t due_ms;
} halloween_scheduler_t;
unsigned halloween_wait_seconds(uint8_t mode, uint32_t random_value);
void halloween_schedule(halloween_scheduler_t *s, uint64_t now_ms, uint32_t random_value);
void halloween_completed(halloween_scheduler_t *s, uint64_t now_ms, uint32_t random_value);
void halloween_cycle(halloween_scheduler_t *s, uint64_t now_ms, uint32_t random_value);
bool halloween_due(const halloween_scheduler_t *s, uint64_t now_ms, bool bluetooth_connected);
#ifdef __cplusplus
}
#endif
