/*
 * Halloween Dial S3 — UI, SD MP3 decode and UART transport to Bluetooth Classic ESP32.
 * This file replaces jpowen898/lvgl_hello_world/main/main.cpp.
 * It uses the actual 360x360 LVGL8 display driver from that repository.
 * ESP32-S3 is NOT a Bluetooth Classic device.
 */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <inttypes.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lvgl.h"
#include "audio_player.h"
#include "display_init.h"
#include "scheduler.h"

static const char *TAG="HalloweenDial";
static const char *TRACK="/sdcard/Witches_Sound_cleaned_louder.mp3";
/* First-boot SD provisioning: linker-embedded copy supplied by build Pi. */
extern "C" const uint8_t _binary_Witches_Sound_cleaned_louder_mp3_start[];
extern "C" const uint8_t _binary_Witches_Sound_cleaned_louder_mp3_end[];
static constexpr uart_port_t LINK_UART=UART_NUM_1;
static constexpr int LINK_TX=40, LINK_RX=39;
static constexpr int KNOB_UP=8, KNOB_DOWN=7;
static halloween_scheduler_t scheduler={.mode=4,.enabled=true,.playing=false,.due_ms=0};
static volatile bool bluetooth_connected=false;
static bool card_ready=false;
static uint8_t volume=65;
static uint32_t audio_rate=48000;
static uint32_t resample_accum=0;
static int64_t stream_start_us=0;
static uint64_t samples_sent=0;
static lv_obj_t *status_label=nullptr,*mode_label=nullptr,*play_button_label=nullptr,*volume_label=nullptr;
static nvs_handle_t nvs_handle;

static uint64_t ms_now(){return (uint64_t)(esp_timer_get_time()/1000);}
static void uart_cmd(uint8_t b){uart_write_bytes(LINK_UART,&b,1);}
static void volume_send(){const uint8_t buf[2]={'V',volume};uart_write_bytes(LINK_UART,buf,sizeof(buf));}
static void settings_save(){nvs_set_u8(nvs_handle,"interval",scheduler.mode);nvs_set_u8(nvs_handle,"volume",volume);nvs_set_u8(nvs_handle,"running",scheduler.enabled?1:0);nvs_commit(nvs_handle);}
static void adjust_volume(int delta){
    int v=(int)volume+delta;
    if(v<0)v=0;if(v>100)v=100;
    volume=(uint8_t)v;
    volume_send();settings_save();
    ESP_LOGI(TAG,"volume=%u",(unsigned)volume);
}
static void knob_task(void*){
    gpio_config_t cfg={};cfg.pin_bit_mask=(1ULL<<KNOB_UP)|(1ULL<<KNOB_DOWN);
    cfg.mode=GPIO_MODE_INPUT;cfg.pull_up_en=GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&cfg));
    bool last_up=true,last_down=true;
    uint64_t up_at=0,down_at=0;
    for(;;){
        bool up=(gpio_get_level((gpio_num_t)KNOB_UP)!=0);
        bool down=(gpio_get_level((gpio_num_t)KNOB_DOWN)!=0);
        uint64_t now=ms_now();
        if(!up&&last_up&&now-up_at>25){adjust_volume(+2);up_at=now;}
        if(!down&&last_down&&now-down_at>25){adjust_volume(-2);down_at=now;}
        last_up=up;last_down=down;
        vTaskDelay(pdMS_TO_TICKS(3));
    }
}
static void status_rx(void*){
    uint8_t b=0;
    for(;;){
        if(uart_read_bytes(LINK_UART,&b,1,pdMS_TO_TICKS(250))==1){
            if(b=='C')bluetooth_connected=true;
            else if(b=='D')bluetooth_connected=false;
        }
    }
}
static void link_init(){
    uart_config_t cfg={.baud_rate=921600,.data_bits=UART_DATA_8_BITS,
        .parity=UART_PARITY_DISABLE,.stop_bits=UART_STOP_BITS_1,
        .flow_ctrl=UART_HW_FLOWCTRL_DISABLE,.source_clk=UART_SCLK_DEFAULT};
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART,2048,4096,0,nullptr,0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART,&cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART,LINK_TX,LINK_RX,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE));
    xTaskCreate(status_rx,"halloween_status",3072,nullptr,6,nullptr);
    ESP_LOGI(TAG,"Inter-chip UART TX40 RX39 @ 921600");
}
static bool verify_or_provision_track() {
    const uint8_t *first = _binary_Witches_Sound_cleaned_louder_mp3_start;
    const uint8_t *last = _binary_Witches_Sound_cleaned_louder_mp3_end;
    const size_t expected = (size_t)(last-first);
    if (expected < 100 || expected > 1024*1024) {
        ESP_LOGE(TAG, "Invalid built-in MP3 asset length: %u", (unsigned)expected);
        return false;
    }
    FILE *existing = fopen(TRACK, "rb");
    if (existing) {
        uint8_t scratch[1024];
        size_t at=0;
        bool matches=true;
        while (at<expected) {
            size_t want=(expected-at)>sizeof(scratch)?sizeof(scratch):(expected-at);
            size_t got=fread(scratch,1,want,existing);
            if (got!=want || memcmp(scratch,first+at,want)!=0) {matches=false;break;}
            at+=got;
        }
        if (matches && fgetc(existing)!=EOF)matches=false;
        fclose(existing);
        if (matches) {ESP_LOGI(TAG,"SD MP3 matches built-in asset: %u bytes",(unsigned)expected);return true;}
        ESP_LOGE(TAG,"Different MP3 already exists: refusing to overwrite %s",TRACK);
        return false;
    }
    /* Write to a temporary path; only rename after complete and sync. */
    static const char *temp="/sdcard/.witches_provision.tmp";
    FILE *out=fopen(temp,"wb");
    if(!out){ESP_LOGE(TAG,"Cannot create SD audio file");return false;}
    bool ok=fwrite(first,1,expected,out)==expected;
    ok=(fflush(out)==0)&&ok;
    ok=(fsync(fileno(out))==0)&&ok;
    ok=(fclose(out)==0)&&ok;
    if (!ok) {remove(temp);ESP_LOGE(TAG,"Failed writing SD MP3");return false;}
    if(rename(temp,TRACK)!=0){remove(temp);ESP_LOGE(TAG,"Failed to finalize SD MP3");return false;}
    ESP_LOGI(TAG,"Provisioned %s (%u bytes)",TRACK,(unsigned)expected);
    return true;
}
static bool sd_mount(){
    spi_bus_config_t spi={};spi.mosi_io_num=3;spi.miso_io_num=5;spi.sclk_io_num=4;
    spi.quadwp_io_num=-1;spi.quadhd_io_num=-1;spi.max_transfer_sz=4096;
    sdmmc_host_t host=SDSPI_HOST_DEFAULT();
    host.slot=SPI3_HOST; // LCD driver owns SPI2_HOST
    ESP_ERROR_CHECK(spi_bus_initialize((spi_host_device_t)host.slot,&spi,SPI_DMA_CH_AUTO));
    sdspi_device_config_t device=SDSPI_DEVICE_CONFIG_DEFAULT();
    device.gpio_cs=GPIO_NUM_2;device.host_id=(spi_host_device_t)host.slot;
    esp_vfs_fat_mount_config_t m={};m.format_if_mount_failed=false;
    m.max_files=4;m.allocation_unit_size=16*1024;
    sdmmc_card_t *card=nullptr;
    esp_err_t rc=esp_vfs_fat_sdspi_mount("/sdcard",&host,&device,&m,&card);
    if(rc!=ESP_OK){ESP_LOGE(TAG,"SD mount failed %s (card must be FAT32, not exFAT)",esp_err_to_name(rc));return false;}
    ESP_LOGI(TAG,"SD mounted; verifying audio file");
    return verify_or_provision_track();
}
static esp_err_t set_audio_clock(uint32_t rate,uint32_t bits_cfg,i2s_slot_mode_t channels){
    (void)bits_cfg;(void)channels;
    audio_rate=rate?rate:48000;
    ESP_LOGI(TAG,"Decoder PCM rate=%" PRIu32, audio_rate);
    return ESP_OK;
}
static esp_err_t amp_noop(AUDIO_PLAYER_MUTE_SETTING setting){(void)setting;return ESP_OK;}
static esp_err_t pcm_uart_write(const void *buf,size_t len,size_t *written,uint32_t timeout_ms){
    (void)timeout_ms;
    /* ESP-audio-player legacy API forces stereo int16 output. Convert it to
       16kHz mono for the 921600 baud UART to the Classic ESP32. */
    const int16_t *pcm=(const int16_t *)buf;
    size_t frames=len/4;
    uint8_t packet[4+512];
    size_t used=0;
    for(size_t i=0;i<frames;i++){
        resample_accum+=16000;
        if(resample_accum<audio_rate)continue;
        resample_accum-=audio_rate;
        int32_t m=((int32_t)pcm[2*i]+(int32_t)pcm[2*i+1])/2;
        if(used==0){packet[0]=0xA5;packet[1]=0x5A;}
        packet[4+used++]=(uint8_t)m;
        packet[4+used++]=(uint8_t)(m>>8);
        samples_sent++;
        if(used==512){
            packet[2]=(uint8_t)used;packet[3]=(uint8_t)(used>>8);
            uart_write_bytes(LINK_UART,packet,4+used);
            used=0;
        }
        /* Pace the decoder ~250ms ahead of Bluetooth's output clock.
           Avoid overrunning the 32KiB receiving PCM ring. */
        int64_t ahead=(int64_t)(samples_sent*1000000ULL/16000ULL) - (esp_timer_get_time()-stream_start_us);
        if(ahead>250000)vTaskDelay(pdMS_TO_TICKS((uint32_t)((ahead-200000)/1000)));
    }
    if(used){packet[2]=(uint8_t)used;packet[3]=(uint8_t)(used>>8);
        uart_write_bytes(LINK_UART,packet,4+used);}
    *written=len;
    return ESP_OK;
}
static void player_events(audio_player_cb_ctx_t *ctx){
    if(ctx->audio_event==AUDIO_PLAYER_CALLBACK_EVENT_IDLE && scheduler.playing){
        uart_cmd('E');
        halloween_completed(&scheduler,ms_now(),esp_random());
        ESP_LOGI(TAG,"MP3 decoded; next delay %u seconds",halloween_wait_seconds(scheduler.mode,0));
    }
    if(ctx->audio_event==AUDIO_PLAYER_CALLBACK_EVENT_UNKNOWN_FILE_TYPE){
        ESP_LOGE(TAG,"MP3 was rejected by decoder, check FF FA frame support");
    }
}
static bool play_file(){
    if(!bluetooth_connected||!card_ready)return false;
    FILE *f=fopen(TRACK,"rb");if(!f)return false;
    uart_cmd('S');resample_accum=0;samples_sent=0;stream_start_us=esp_timer_get_time();
    scheduler.playing=true;scheduler.due_ms=0;
    esp_err_t rc=audio_player_play(f);
    if(rc!=ESP_OK){fclose(f);scheduler.playing=false;uart_cmd('X');return false;}
    ESP_LOGI(TAG,"Playing %s",TRACK);
    return true;
}
static const char *mode_text(){
    static const char *modes[]={"10 sec","20 sec","30 sec","40 sec","RANDOM"};
    return modes[scheduler.mode];
}
static void on_toggle(lv_event_t *evt){
    if(lv_event_get_code(evt)!=LV_EVENT_CLICKED)return;
    scheduler.enabled=!scheduler.enabled;
    if(!scheduler.enabled){if(scheduler.playing){audio_player_stop();uart_cmd('X');scheduler.playing=false;}scheduler.due_ms=0;}
    else halloween_schedule(&scheduler,ms_now(),esp_random());
    settings_save();
}
static void on_interval(lv_event_t *evt){
    if(lv_event_get_code(evt)!=LV_EVENT_CLICKED)return;
    halloween_cycle(&scheduler,ms_now(),esp_random());settings_save();
    ESP_LOGI(TAG,"Interval %s",mode_text());
}
static void on_pair(lv_event_t *evt){
    if(lv_event_get_code(evt)==LV_EVENT_CLICKED){uart_cmd('P');ESP_LOGI(TAG,"Pair requested: ITSBO-513");}
}
static void ui_refresh(lv_timer_t*){
    static char buf[64];
    lv_label_set_text(status_label,bluetooth_connected?"ITSBO-513  CONNECTED":"ITSBO-513  DISCONNECTED");
    lv_label_set_text(mode_label,mode_text());
    lv_label_set_text(play_button_label,scheduler.enabled?"PAUSE":"PLAY");
    snprintf(buf,sizeof(buf),"VOLUME %u%%",(unsigned)volume);lv_label_set_text(volume_label,buf);
}
static lv_obj_t *make_button(lv_obj_t *parent,const char *caption,int x,int y,int w,int h,lv_event_cb_t cb){
    lv_obj_t *button=lv_btn_create(parent);
    lv_obj_set_size(button,w,h);lv_obj_set_pos(button,x,y);
    lv_obj_set_style_radius(button,LV_RADIUS_CIRCLE,0);
    lv_obj_add_event_cb(button,cb,LV_EVENT_CLICKED,nullptr);
    lv_obj_t *label=lv_label_create(button);lv_label_set_text(label,caption);
    lv_obj_center(label);return label;
}
static void ui_create(){
    lv_obj_t *root=lv_scr_act();
    lv_obj_set_style_bg_color(root,lv_color_hex(0x101A24),0);
    lv_obj_t *title=lv_label_create(root);lv_label_set_text(title,"HALLOWEEN");
    lv_obj_set_style_text_color(title,lv_color_hex(0xDFAC73),0);
    lv_obj_align(title,LV_ALIGN_TOP_MID,0,37);
    status_label=lv_label_create(root);lv_obj_align(status_label,LV_ALIGN_TOP_MID,0,72);
    play_button_label=make_button(root,"PAUSE",108,104,144,80,on_toggle);
    mode_label=make_button(root,"RANDOM",61,203,112,51,on_interval);
    make_button(root,"PAIR",189,203,112,51,on_pair);
    volume_label=lv_label_create(root);lv_obj_align(volume_label,LV_ALIGN_BOTTOM_MID,0,-52);
    lv_timer_create(ui_refresh,200,nullptr);ui_refresh(nullptr);
}
extern "C" void app_main(void){
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nvs_open("halloween",NVS_READWRITE,&nvs_handle));
    nvs_get_u8(nvs_handle,"interval",&scheduler.mode);if(scheduler.mode>4)scheduler.mode=4;
    nvs_get_u8(nvs_handle,"volume",&volume);if(volume>100)volume=65;
    uint8_t enabled=1;nvs_get_u8(nvs_handle,"running",&enabled);scheduler.enabled=enabled!=0;
    link_init();volume_send();card_ready=sd_mount();
    audio_player_config_t ap={};
    ap.mute_fn=amp_noop;ap.clk_set_fn=set_audio_clock;ap.write_fn=pcm_uart_write;
    ap.priority=5;ap.coreID=1;ap.force_stereo=true;
    ESP_ERROR_CHECK(audio_player_new(ap));
    audio_player_callback_register(player_events,nullptr);
    display_init();ui_create();
    xTaskCreate(knob_task,"halloween_knob",3072,nullptr,4,nullptr);
    halloween_schedule(&scheduler,ms_now(),esp_random());
    for(;;){
        if(!bluetooth_connected && scheduler.playing){audio_player_stop();uart_cmd('X');scheduler.playing=false;scheduler.due_ms=0;}
        if(bluetooth_connected && scheduler.enabled && !scheduler.playing && !scheduler.due_ms){
            halloween_schedule(&scheduler,ms_now(),esp_random());
        }
        if(halloween_due(&scheduler,ms_now(),bluetooth_connected) && !play_file()) halloween_schedule(&scheduler,ms_now(),esp_random());
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}