#include "bridge.h"
#include <string.h>
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
/* Measured inter-MCU link: S3 TX40 -> ESP32 RX23; ESP32 TX18 -> S3 RX39 */
#define LINK_UART UART_NUM_1
#define UART_RX 23
#define UART_TX 18
#define RING_SAMPLES 16384
#define HEADER 0xA5
#define MARKER 0x5A
static const char *TAG="HalloweenClassic";
static int16_t samples[RING_SAMPLES];
static volatile unsigned read_index=0,write_index=0,count=0;
static portMUX_TYPE spin=portMUX_INITIALIZER_UNLOCKED;
static volatile bool paired=false, request_pair=false, stream_active=false, stream_ending=false;
static volatile uint8_t volume=65;
static uint32_t interp_phase=0;
static int16_t last_sample=0;
static uint64_t dropped=0;
static void reset_ring(void) {
    portENTER_CRITICAL(&spin);
    read_index=write_index=count=0;
    portEXIT_CRITICAL(&spin);
    interp_phase=0;last_sample=0;
}
static void store_sample(int16_t v) {
    portENTER_CRITICAL(&spin);
    if(count<RING_SAMPLES){samples[write_index]=v;write_index=(write_index+1)%RING_SAMPLES;count++;}
    else dropped++;
    portEXIT_CRITICAL(&spin);
}
static int16_t pull_sample(void) {
    int16_t v=0;
    portENTER_CRITICAL(&spin);
    if(count){v=samples[read_index];read_index=(read_index+1)%RING_SAMPLES;count--;}
    if(!count && stream_ending){stream_active=false;stream_ending=false;}
    portEXIT_CRITICAL(&spin);
    return v;
}
static void receiver(void *unused) {
    (void)unused;
    /* Frame: A5 5A lenL lenH followed by little-endian 16-kHz mono PCM. */
    enum {WAIT,MARK,LEN_LO,LEN_HI,DATA,VOLUME} state=WAIT;
    uint16_t left=0,len_lo=0;
    bool low_byte=true;
    uint8_t sample_lo=0,b;
    int64_t last_status=0;
    while(1) {
        if(uart_read_bytes(LINK_UART,&b,1,pdMS_TO_TICKS(20))==1) {
            switch(state) {
                case WAIT:
                    if(b==HEADER)state=MARK;
                    else if(b=='P')request_pair=true;
                    else if(b=='S'){reset_ring();stream_ending=false;stream_active=true;}
                    else if(b=='E'){stream_ending=true;}
                    else if(b=='X'){stream_active=false;stream_ending=false;reset_ring();}
                    else if(b=='V')state=VOLUME;
                    break;
                case VOLUME: volume=b>100?100:b;state=WAIT;break;
                case MARK: state=(b==MARKER)?LEN_LO:WAIT;break;
                case LEN_LO: len_lo=b;state=LEN_HI;break;
                case LEN_HI:
                    left=(uint16_t)(len_lo | ((uint16_t)b<<8));
                    if(!left || left>512 || (left & 1u)) {state=WAIT;break;}
                    low_byte=true;state=DATA;break;
                case DATA:
                    if(low_byte){sample_lo=b;low_byte=false;}
                    else {store_sample((int16_t)(sample_lo|((uint16_t)b<<8)));low_byte=true;}
                    if(--left==0)state=WAIT;
                    break;
            }
        }
        int64_t now=esp_timer_get_time();
        if(now-last_status>1000000){
            uint8_t status=paired?'C':'D';
            uart_write_bytes(LINK_UART,&status,1);
            last_status=now;
        }
    }
}
void hw_bridge_start(void) {
    uart_config_t cfg={.baud_rate=921600,.data_bits=UART_DATA_8_BITS,.parity=UART_PARITY_DISABLE,.stop_bits=UART_STOP_BITS_1,.flow_ctrl=UART_HW_FLOWCTRL_DISABLE,.source_clk=UART_SCLK_DEFAULT};
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART,4096,2048,0,NULL,0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART,&cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART,UART_TX,UART_RX,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE));
    xTaskCreate(receiver,"halloween_rx",4096,NULL,6,NULL);
    ESP_LOGI(TAG,"UART bridge ready; RX23 TX18 @921600");
}
void hw_bridge_set_connected(bool connected) {paired=connected;if(!connected){stream_active=false;reset_ring();}}
bool hw_bridge_take_pair_request(void) {bool was=request_pair;request_pair=false;return was;}
int32_t hw_bridge_fill_pcm(uint8_t *data,int32_t len) {
    if(!data||len<=0)return 0;
    /* ESP-IDF A2DP source expects interleaved stereo 16-bit PCM, 44.1kHz.
       We approximate 16kHz mono -> 44.1kHz stereo with zero-order hold.
       Clock-rate negotiation and underflow handling require hardware testing. */
    int frames=len/4;
    int16_t *out=(int16_t*)data;
    for(int i=0;i<frames;i++) {
        if(paired && stream_active){
            interp_phase+=16000;
            if(interp_phase>=44100){interp_phase-=44100;last_sample=pull_sample();}
        } else last_sample=0;
        int32_t scaled=(int32_t)last_sample*volume/100;
        out[i*2]=(int16_t)scaled;
        out[i*2+1]=(int16_t)scaled;
    }
    if(len%4)memset(data+frames*4,0,(size_t)(len%4));
    return len;
}