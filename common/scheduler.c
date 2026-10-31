#include "scheduler.h"
unsigned halloween_wait_seconds(uint8_t mode, uint32_t random_value) {
    if (mode >= 4) return 10u + random_value % 31u;
    return (unsigned)(mode + 1u) * 10u;
}
void halloween_schedule(halloween_scheduler_t *s, uint64_t now_ms, uint32_t random_value) {
    if (!s->enabled || s->playing) {s->due_ms=0; return;}
    s->due_ms=now_ms+(uint64_t)halloween_wait_seconds(s->mode,random_value)*1000u;
}
void halloween_completed(halloween_scheduler_t *s, uint64_t now_ms, uint32_t random_value) {
    s->playing=false;
    halloween_schedule(s,now_ms,random_value);
}
void halloween_cycle(halloween_scheduler_t *s, uint64_t now_ms, uint32_t random_value) {
    s->mode=(uint8_t)((s->mode+1u)%5u);
    if(!s->playing) halloween_schedule(s,now_ms,random_value);
}
bool halloween_due(const halloween_scheduler_t *s, uint64_t now_ms, bool connected) {
    return s->enabled && connected && !s->playing && s->due_ms!=0 && now_ms >= s->due_ms;
}
