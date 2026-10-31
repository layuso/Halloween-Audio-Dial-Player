#include <assert.h>
#include <stdio.h>
#include "scheduler.h"
int main(void) {
    halloween_scheduler_t s = {.mode=0,.enabled=true};
    for(int i=0;i<4;i++)assert(halloween_wait_seconds(i,0)==10u*(unsigned)(i+1));
    for(unsigned x=0;x<100000;x++) {unsigned d=halloween_wait_seconds(4,x);assert(d>=10&&d<=40);}
    halloween_schedule(&s,1000,0);assert(s.due_ms==11000);
    assert(!halloween_due(&s,11000,false));assert(halloween_due(&s,11000,true));
    s.playing=true; assert(!halloween_due(&s,12000,true));
    halloween_completed(&s,12000,10);assert(s.due_ms==22000);
    for(int i=0;i<4;i++)halloween_cycle(&s,0,0);
    assert(s.mode==4 && s.due_ms==10000);
    halloween_cycle(&s,0,0);assert(s.mode==0 && s.due_ms==10000);
    s.enabled=false;halloween_schedule(&s,999,0);assert(s.due_ms==0);
    puts("PASS: interval schedule, no overlap, BT gate, mode wrap, random bounds");
}
