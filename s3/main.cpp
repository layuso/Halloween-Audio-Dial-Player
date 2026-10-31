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
#include "freertos/event_groups.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/sdmmc_host.h"
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
extern "C" {
#include "user_encoder_bsp.h"
}

/* The larger fonts are enabled via LVGL Kconfig where available. */
static const lv_font_t *font_heading(){
#if LV_FONT_MONTSERRAT_20
    return &lv_font_montserrat_20;
#else
    return LV_FONT_DEFAULT;
#endif
}
static const lv_font_t *font_action(){
#if LV_FONT_MONTSERRAT_24
    return &lv_font_montserrat_24;
#elif LV_FONT_MONTSERRAT_20
    return &lv_font_montserrat_20;
#else
    return LV_FONT_DEFAULT;
#endif
}
static const char *TAG="HalloweenDial";
static const char *TRACK="/sdcard/Witches_Sound_cleaned_louder.mp3";
/* First-boot SD provisioning: linker-embedded copy supplied by build Pi. */
extern "C" const uint8_t _binary_Witches_Sound_cleaned_louder_mp3_start[];
extern "C" const uint8_t _binary_Witches_Sound_cleaned_louder_mp3_end[];
static constexpr uart_port_t LINK_UART=UART_NUM_1;
static constexpr int LINK_TX=40, LINK_RX=39;
static halloween_scheduler_t scheduler={.mode=4,.enabled=true,.playing=false,.due_ms=0};
static volatile bool bluetooth_connected=false;
static volatile uint64_t last_link_rx_ms=0;
static volatile bool pair_requested=false;
static uint64_t pair_request_ms=0;
static bool card_ready=false;
static bool flash_audio_ready=false;
static const char *sd_state="SD NOT INITIALIZED";
static uint8_t volume=65;
static uint32_t audio_rate=48000;
static uint32_t resample_accum=0;
static int64_t stream_start_us=0;
static uint64_t samples_sent=0;
static lv_obj_t *status_label=nullptr,*mode_label=nullptr,*play_button_label=nullptr,*volume_label=nullptr;
static lv_obj_t *pair_button_label=nullptr,*countdown_label=nullptr;
static volatile bool volume_dirty=false;
static uint64_t volume_changed_ms=0;
static nvs_handle_t halloween_nvs;

static uint64_t ms_now(){return (uint64_t)(esp_timer_get_time()/1000);}
static bool uart_cmd(uint8_t b){
    int queued=uart_write_bytes(LINK_UART,&b,1);
    if(queued!=1) ESP_LOGE(TAG,"UART TX40 failed to queue command 0x%02X (return %d)",
                           (unsigned)b,queued);
    return queued==1;
}
static void volume_send(){const uint8_t buf[2]={'V',volume};uart_write_bytes(LINK_UART,buf,sizeof(buf));}
static void settings_save(){nvs_set_u8(halloween_nvs,"interval",scheduler.mode);nvs_set_u8(halloween_nvs,"volume",volume);nvs_set_u8(halloween_nvs,"running",scheduler.enabled?1:0);nvs_commit(halloween_nvs);}
static void adjust_volume(int delta){
    int v=(int)volume+delta;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    volume=(uint8_t)v;
    volume_send();
    volume_dirty=true;
    volume_changed_ms=ms_now();
    ESP_LOGI(TAG,"volume=%u",(unsigned)volume);
}
/* Use the board's encoder BSP (3 ms sampling + per-channel debounce).
 * GPIO8 is right/volume+, GPIO7 is left/volume-. The old direct falling-edge
 * handler missed some counterclockwise detents and wrote NVS on every pulse. */
static void knob_task(void*){
    user_encoder_init();
    ESP_LOGI(TAG,"Waveshare rotary encoder active (right/up, left/down)");
    for(;;){
        EventBits_t events=xEventGroupWaitBits(knob_even_, BIT0 | BIT1,
                                               pdTRUE,pdFALSE,pdMS_TO_TICKS(50));
        if((events & BIT0) && !(events & BIT1)) adjust_volume(-2);
        else if((events & BIT1) && !(events & BIT0)) adjust_volume(+2);
        else if((events & (BIT0|BIT1))==(BIT0|BIT1))
            ESP_LOGW(TAG,"Both rotary directions reported in one interval");
        if(volume_dirty && ms_now()-volume_changed_ms>600){
            volume_dirty=false;
            settings_save();
        }
    }
}
static void status_rx(void*){
    uint8_t b=0;
    unsigned heartbeats=0, unexpected=0;
    int64_t last_report_us=esp_timer_get_time();
    for(;;){
        int received=uart_read_bytes(LINK_UART,&b,1,pdMS_TO_TICKS(250));
        if(received==1){
            if(b=='C' || b=='D'){
                bluetooth_connected=(b=='C');
                last_link_rx_ms=ms_now();
                if(bluetooth_connected) pair_requested=false;
                heartbeats++;
                if(heartbeats==1 || (heartbeats%30)==0)
                    ESP_LOGI(TAG,"UART RX39 heartbeat #%u: %c (Bluetooth %s)",
                             heartbeats,(char)b,b=='C'?"connected":"disconnected");
            } else if(unexpected++ < 12){
                ESP_LOGW(TAG,"UART RX39 unexpected byte 0x%02X (baud/pin/framing diagnostic)",
                         (unsigned)b);
            }
        }
        int64_t now=esp_timer_get_time();
        if(now-last_report_us >= 10000000){
            bool online=last_link_rx_ms && ms_now()-last_link_rx_ms<3500;
            if(!online)
                ESP_LOGW(TAG,"UART RX39: no valid Classic heartbeat; GPIO39 level=%d, bytes seen=%u, valid heartbeats=%u",
                         gpio_get_level((gpio_num_t)LINK_RX),unexpected,heartbeats);
            last_report_us=now;
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
        if (matches) {sd_state="SD AUDIO READY";ESP_LOGI(TAG,"SD MP3 matches built-in asset: %u bytes",(unsigned)expected);return true;}
        ESP_LOGE(TAG,"Different MP3 already exists: refusing to overwrite %s",TRACK);
        sd_state="AUDIO FILE DIFFERS";
        return false;
    }
    /* Write to a temporary path; only rename after complete and sync. */
    static const char *temp="/sdcard/.witches_provision.tmp";
    FILE *out=fopen(temp,"wb");
    if(!out){sd_state="SD AUDIO WRITE FAILED";ESP_LOGE(TAG,"Cannot create SD audio file");return false;}
    bool ok=fwrite(first,1,expected,out)==expected;
    ok=(fflush(out)==0)&&ok;
    ok=(fsync(fileno(out))==0)&&ok;
    ok=(fclose(out)==0)&&ok;
    if (!ok) {remove(temp);sd_state="SD AUDIO WRITE FAILED";ESP_LOGE(TAG,"Failed writing SD MP3");return false;}
    if(rename(temp,TRACK)!=0){remove(temp);sd_state="SD AUDIO RENAME FAILED";ESP_LOGE(TAG,"Failed to finalize SD MP3");return false;}
    sd_state="SD AUDIO READY";
    ESP_LOGI(TAG,"Provisioned %s (%u bytes)",TRACK,(unsigned)expected);
    return true;
}
static bool sd_mount(){
    /* Waveshare ESP32-S3-Knob-Touch-LCD-1.8 TF slot is wired for SDMMC:
       CLK=4 CMD=3 D0=5 D1=6 D2=42 D3=2.
       The old SDSPI initialization was unreliable on this board. */
    sdmmc_host_t host=SDMMC_HOST_DEFAULT();
    host.max_freq_khz=SDMMC_FREQ_DEFAULT; // 20 MHz; prioritize reliable startup
    sdmmc_slot_config_t slot=SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width=4;
    slot.clk=GPIO_NUM_4;
    slot.cmd=GPIO_NUM_3;
    slot.d0=GPIO_NUM_5;
    slot.d1=GPIO_NUM_6;
    slot.d2=GPIO_NUM_42;
    slot.d3=GPIO_NUM_2;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_mount_config_t m={};
    m.format_if_mount_failed=false; // Never format or erase user's SD card.
    m.max_files=4;
    m.allocation_unit_size=16*1024;
    sdmmc_card_t *card=nullptr;
    ESP_LOGI(TAG,"Mounting TF card: SDMMC 4-bit CLK4 CMD3 D0=5 D1=6 D2=42 D3=2");
    esp_err_t rc=esp_vfs_fat_sdmmc_mount("/sdcard",&host,&slot,&m,&card);
    if(rc!=ESP_OK){
        sd_state="SD MOUNT FAILED";
        ESP_LOGE(TAG,"SDMMC mount failed: %s (0x%x); check card insertion and FAT32 format",
                 esp_err_to_name(rc),(unsigned)rc);
        return false;
    }
    ESP_LOGI(TAG,"SDMMC card mounted");
    sdmmc_card_print_info(stdout,card);
    sd_state="AUDIO FILE UNAVAILABLE";
    return verify_or_provision_track();
}
static esp_err_t set_audio_clock(uint32_t rate,uint32_t bits_cfg,i2s_slot_mode_t channels){
    (void)bits_cfg;(void)channels;
    audio_rate=rate?rate:48000;
    ESP_LOGI(TAG,"Decoder PCM rate=%" PRIu32, audio_rate);
    return ESP_OK;
}
static esp_err_t amp_noop(AUDIO_PLAYER_MUTE_SETTING setting){(void)setting;return ESP_OK;}
static esp_err_t pcm_uart_write(void *buf,size_t len,size_t *written,uint32_t timeout_ms){
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
    if(!bluetooth_connected || (!card_ready && !flash_audio_ready)) return false;
    FILE *f=card_ready ? fopen(TRACK,"rb") : nullptr;
    bool from_flash=(f==nullptr);
    if(from_flash && flash_audio_ready) {
        /* The MP3 is already linked into S3 application flash. A read-only
           memory FILE avoids making sound effects depend on a removable SD. */
        const uint8_t *first=_binary_Witches_Sound_cleaned_louder_mp3_start;
        const size_t length=(size_t)(_binary_Witches_Sound_cleaned_louder_mp3_end-first);
        f=fmemopen((void*)first,length,"rb");
        if(!f) ESP_LOGE(TAG,"Cannot open embedded audio stream (fmemopen)");
    }
    if(!f) return false;
    uart_cmd('S');resample_accum=0;samples_sent=0;stream_start_us=esp_timer_get_time();
    scheduler.playing=true;scheduler.due_ms=0;
    esp_err_t rc=audio_player_play(f);
    if(rc!=ESP_OK){fclose(f);scheduler.playing=false;uart_cmd('X');return false;}
    ESP_LOGI(TAG,"Playing Halloween MP3 from %s",from_flash?"embedded flash":"SD card");
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
    if(lv_event_get_code(evt)!=LV_EVENT_CLICKED)return;
    pair_requested=true;
    pair_request_ms=ms_now();
    bool queued=uart_cmd('P');
    ESP_LOGI(TAG,"PAIR command %s on UART TX40; physical delivery not yet confirmed",
             queued?"queued":"FAILED");
}
static void ui_refresh(lv_timer_t*){
    uint64_t now=ms_now();
    bool link_ready=last_link_rx_ms && now-last_link_rx_ms<3500;
    lv_label_set_text(status_label,!link_ready?"LINK OFFLINE":
                      bluetooth_connected?"SPEAKER CONNECTED":
                      pair_requested && now-pair_request_ms<15000?"SEARCHING FOR SPEAKER":
                      "SPEAKER NOT CONNECTED");
    lv_obj_set_style_text_color(status_label,
        lv_color_hex(!link_ready?0xEAA44A:bluetooth_connected?0x8EE2B8:0xF1BB74),0);
    if(pair_requested && now-pair_request_ms>15000)pair_requested=false;
    lv_label_set_text(mode_label,mode_text());
    lv_label_set_text(play_button_label,scheduler.enabled?"PAUSE":"START");
    lv_label_set_text(pair_button_label,bluetooth_connected?"REPAIR":"PAIR");
    char buf[64];
    snprintf(buf,sizeof(buf),"VOLUME  %u%%",(unsigned)volume);
    lv_label_set_text(volume_label,buf);
    if(!card_ready && !flash_audio_ready) snprintf(buf,sizeof(buf),"%s",sd_state);
    else if(scheduler.playing) snprintf(buf,sizeof(buf),"PLAYING SOUND");
    else if(!scheduler.enabled) snprintf(buf,sizeof(buf),"SCHEDULE PAUSED");
    else if(!link_ready) snprintf(buf,sizeof(buf),
             card_ready?"SD AUDIO READY - CHECK LINK":"MP3 READY - CHECK LINK");
    else if(!bluetooth_connected) snprintf(buf,sizeof(buf),
             "AUDIO READY - PAIR SPEAKER");
    else if(scheduler.due_ms>now)
        snprintf(buf,sizeof(buf),"NEXT SOUND IN %llus",
                 (unsigned long long)((scheduler.due_ms-now+999)/1000));
    else snprintf(buf,sizeof(buf),card_ready?"SD AUDIO READY":"EMBEDDED MP3 READY");
    lv_label_set_text(countdown_label,buf);
}
static lv_obj_t *make_button(lv_obj_t *parent,const char *caption,int x,int y,int w,int h,
                              uint32_t bg,uint32_t border,lv_event_cb_t cb,bool primary){
    lv_obj_t *button=lv_btn_create(parent);
    lv_obj_set_size(button,w,h);
    lv_obj_set_pos(button,x,y);
    lv_obj_set_style_radius(button,primary?48:23,0);
    lv_obj_set_style_bg_color(button,lv_color_hex(bg),0);
    lv_obj_set_style_bg_opa(button,LV_OPA_COVER,0);
    lv_obj_set_style_border_width(button,1,0);
    lv_obj_set_style_border_color(button,lv_color_hex(border),0);
    lv_obj_set_style_border_opa(button,LV_OPA_COVER,0);
    lv_obj_set_style_shadow_color(button,lv_color_hex(primary?0xE67622:0x37264E),0);
    lv_obj_set_style_shadow_width(button,primary?15:5,0);
    lv_obj_set_style_shadow_opa(button,primary?LV_OPA_40:LV_OPA_20,0);
    lv_obj_set_style_pad_all(button,0,0);
    lv_obj_add_event_cb(button,cb,LV_EVENT_CLICKED,nullptr);
    lv_obj_t *label=lv_label_create(button);
    lv_label_set_text(label,caption);
    lv_obj_set_style_text_color(label,lv_color_hex(primary?0x120E15:0xF1EBFF),0);
    if(primary)lv_obj_set_style_text_font(label,font_action(),0);
    lv_obj_set_style_text_letter_space(label,primary?3:1,0);
    lv_obj_center(label);
    return label;
}
static void ui_create(){
    lv_obj_t *root=lv_scr_act();
    lv_obj_set_style_bg_color(root,lv_color_hex(0x0B0B10),0);
    lv_obj_set_style_bg_opa(root,LV_OPA_COVER,0);
    lv_obj_set_style_border_width(root,0,0);
    lv_obj_set_style_radius(root,0,0);
    lv_obj_clear_flag(root,LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title=lv_label_create(root);
    lv_label_set_text(title,"H A L L O W E E N");
    lv_obj_set_style_text_color(title,lv_color_hex(0xFFAD62),0);
    lv_obj_set_style_text_font(title,font_heading(),0);
    lv_obj_align(title,LV_ALIGN_TOP_MID,0,31);

    lv_obj_t *subtitle=lv_label_create(root);
    lv_label_set_text(subtitle,"SOUND MACHINE");
    lv_obj_set_style_text_color(subtitle,lv_color_hex(0x9C91AE),0);
    lv_obj_set_style_text_letter_space(subtitle,2,0);
    lv_obj_align(subtitle,LV_ALIGN_TOP_MID,0,58);

    status_label=lv_label_create(root);
    lv_obj_align(status_label,LV_ALIGN_TOP_MID,0,87);

    play_button_label=make_button(root,"PAUSE",100,117,160,83,
                                  0xF18A30,0xFFC184,on_toggle,true);
    mode_label=make_button(root,"RANDOM",44,217,132,51,
                           0x322343,0x7E538B,on_interval,false);
    pair_button_label=make_button(root,"PAIR",184,217,132,51,
                                  0x322343,0x7E538B,on_pair,false);

    volume_label=lv_label_create(root);
    lv_obj_set_style_text_color(volume_label,lv_color_hex(0xF2E9FD),0);
    lv_obj_set_style_text_letter_space(volume_label,1,0);
    lv_obj_align(volume_label,LV_ALIGN_TOP_MID,0,283);

    countdown_label=lv_label_create(root);
    lv_obj_set_style_text_color(countdown_label,lv_color_hex(0xB5AAC6),0);
    lv_obj_align(countdown_label,LV_ALIGN_TOP_MID,0,311);
    lv_timer_create(ui_refresh,200,nullptr);
    ui_refresh(nullptr);
}
extern "C" void app_main(void){
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nvs_open("halloween",NVS_READWRITE,&halloween_nvs));
    nvs_get_u8(halloween_nvs,"interval",&scheduler.mode);if(scheduler.mode>4)scheduler.mode=4;
    nvs_get_u8(halloween_nvs,"volume",&volume);if(volume>100)volume=65;
    uint8_t enabled=1;nvs_get_u8(halloween_nvs,"running",&enabled);scheduler.enabled=enabled!=0;
    /* Verify the embedded MP3 before trying the removable SD card.
       A failing SD mount must not disable the sound machine. */
    const size_t asset_size=(size_t)(
        _binary_Witches_Sound_cleaned_louder_mp3_end-
        _binary_Witches_Sound_cleaned_louder_mp3_start);
    flash_audio_ready=(asset_size>=100 && asset_size<=1024*1024);
    ESP_LOGI(TAG,"Embedded Halloween MP3: %u bytes (%s)",
             (unsigned)asset_size,flash_audio_ready?"READY":"INVALID");
    link_init();volume_send();card_ready=sd_mount();
    if(!card_ready && flash_audio_ready)
        ESP_LOGW(TAG,"SD unavailable (%s); playing from embedded flash instead",sd_state);
    audio_player_config_t ap={};
    ap.mute_fn=amp_noop;ap.clk_set_fn=set_audio_clock;ap.write_fn=pcm_uart_write;
    ap.priority=5;ap.coreID=1;
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