#!/usr/bin/env python3
"""Produce A2DP Source project from installed ESP-IDF example with Bluetooth bridge."""
from pathlib import Path
import os,re,shutil,sys
idf=Path(os.environ.get('IDF_PATH',''))
src=idf/'examples/bluetooth/bluedroid/classic_bt/a2dp_source'
dest=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else Path('build-classic').resolve()
if not (src/'main/main.c').exists():sys.exit('Missing IDF A2DP source example. Export IDF_PATH first.')
if dest.exists():sys.exit(f'Refusing to overwrite {dest}')
shutil.copytree(src,dest)
shutil.copy(Path(__file__).with_name('bridge.c'),dest/'main/bridge.c')
shutil.copy(Path(__file__).with_name('bridge.h'),dest/'main/bridge.h')
main=dest/'main/main.c'
s=main.read_text()
s=s.replace('#include "esp_avrc_api.h"','#include "esp_avrc_api.h"\n#include "bridge.h"')
# The original Espressif A2DP example configures its controller for BR/EDR-only.
# Preserve that setting: without it the controller config defaults to BTDM,
# but app_main enables CLASSIC_BT and fails with ESP_ERR_INVALID_ARG.
needle='    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();'
assert needle in s
s=s.replace(needle,needle+'''\n    ESP_LOGI(BT_AV_TAG, "Bluetooth controller init mode=%d; requested Classic=%d",
             (int)bt_cfg.mode, (int)ESP_BT_MODE_CLASSIC_BT);''',1)
needle='''    if (esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "%s enable controller failed", __func__);
        return;
    }'''
assert needle in s
s=s.replace(needle,'''    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "Bluetooth controller enable failed: %s (0x%x); config mode=%d, requested=%d",
                 esp_err_to_name(ret), (unsigned)ret, (int)bt_cfg.mode,
                 (int)ESP_BT_MODE_CLASSIC_BT);
        return;
    }
    ESP_LOGI(BT_AV_TAG, "Bluetooth Classic controller ENABLED");''',1)

# The reference example rejects speakers without COD=RENDERING or a full EIR name.
# Real A2DP sinks often advertise the name only in BDNAME; match both safely.
scan_start=s.index('static void filter_inquiry_scan_result(esp_bt_gap_cb_param_t *param)\n{')
scan_end=s.index('static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)\n{',scan_start)
s=s[:scan_start]+'''static void filter_inquiry_scan_result(esp_bt_gap_cb_param_t *param)
{
    char addr[18] = {0};
    uint8_t name[ESP_BT_GAP_MAX_BDNAME_LEN + 1] = {0};
    uint8_t *eir = NULL;
    for (int i = 0; i < param->disc_res.num_prop; ++i) {
        esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
        if (p->type == ESP_BT_GAP_DEV_PROP_EIR && p->val) {
            eir = (uint8_t*)p->val;
        } else if (p->type == ESP_BT_GAP_DEV_PROP_BDNAME && p->val && p->len) {
            size_t n = p->len < ESP_BT_GAP_MAX_BDNAME_LEN
                         ? p->len : ESP_BT_GAP_MAX_BDNAME_LEN;
            memcpy(name, p->val, n);
            name[n] = 0;
        }
    }
    if (!name[0] && eir) get_name_from_eir(eir, name, NULL);
    if (!name[0]) return;
    ESP_LOGI(BT_AV_TAG, "Nearby BT: %s (%s)", name,
             bda2str(param->disc_res.bda, addr, sizeof(addr)));
    if (strcmp((const char*)name, remote_device_name) != 0) return;
    memcpy(s_peer_bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
    memcpy(s_peer_bdname, name, sizeof(s_peer_bdname));
    s_a2d_state = APP_AV_STATE_DISCOVERED;
    ESP_LOGI(BT_AV_TAG, "MATCH: %s; connecting A2DP", remote_device_name);
    esp_bt_gap_cancel_discovery();
}

'''+s[scan_end:]

assert s.count('static int32_t bt_app_a2d_data_cb(uint8_t *data, int32_t len)\n{') == 1
s,n=re.subn(r'static int32_t bt_app_a2d_data_cb\(uint8_t \*data, int32_t len\)\s*\{.*?\n\}',
    'static int32_t bt_app_a2d_data_cb(uint8_t *data, int32_t len)\n{ return hw_bridge_fill_pcm(data, len); }',s,count=1,flags=re.S)
assert n==1
# Propagate every connection event to the companion UART bridge.
needle='static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)\n{'
assert needle in s
s=s.replace(needle,needle+'\n    if(event == ESP_A2D_CONNECTION_STATE_EVT) hw_bridge_set_connected(param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED);',1)
# Build on the working factory A2DP discovery state machine; user can ask for re-pair from touchscreen.
needle='static void bt_app_a2d_heart_beat(TimerHandle_t arg)\n{'
assert needle in s
s=s.replace(needle,needle+'''
    /* Deliver the request into the A2DP app task; do not mutate its state
       or initiate Bluetooth operations in FreeRTOS timer callback context. */
    if (hw_bridge_take_pair_request()) {
        bt_app_work_dispatch(bt_app_repair_hdlr, BT_APP_HEART_BEAT_EVT, NULL, 0, NULL, NULL);
    }
''',1)

# Re-pairing must disconnect asynchronously before restarting discovery.
s=s.replace('static const char remote_device_name[] = CONFIG_EXAMPLE_PEER_DEVICE_NAME;',
'''static const char remote_device_name[] = CONFIG_EXAMPLE_PEER_DEVICE_NAME;
static bool s_repair_after_disconnect = false;
''',1)
needle='static void bt_app_a2d_heart_beat(TimerHandle_t arg)\n{'
assert needle in s
s=s.replace(needle,'''static void bt_app_repair_hdlr(uint16_t event, void *arg)
{
    (void)event; (void)arg;
    ESP_LOGI(BT_AV_TAG, "User requested rediscovery of %s", remote_device_name);
    if (s_a2d_state == APP_AV_STATE_CONNECTED) {
        s_repair_after_disconnect = true;
        s_a2d_state = APP_AV_STATE_DISCONNECTING;
        esp_a2d_source_disconnect(s_peer_bda);
    } else if (s_a2d_state == APP_AV_STATE_DISCONNECTING) {
        s_repair_after_disconnect = true;
    } else {
        esp_bt_gap_cancel_discovery();
        s_a2d_state = APP_AV_STATE_DISCOVERING;
        esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
    }
}

'''+needle,1)
# The sample disconnects after 100s even when audio is needed later.
# This dial must remain bonded and connected while the scheduled effects play.
start=s.index('    case APP_AV_MEDIA_STATE_STARTED: {',s.index('static void bt_app_av_media_proc('))
end=s.index('    case APP_AV_MEDIA_STATE_STOPPING:', start)
s=s[:start]+'''    case APP_AV_MEDIA_STATE_STARTED: {
        /* Maintain continuous A2DP connection; silence between sound cues. */
        break;
    }
'''+s[end:]
# Once disconnect completes, resume discovery if user pressed REPAIR.
start=s.index('static void bt_app_av_state_disconnecting_hdlr(uint16_t event, void *param)\n{')
loc=s.index('s_a2d_state =  APP_AV_STATE_UNCONNECTED;',start)
s=s[:loc]+'''s_a2d_state = APP_AV_STATE_UNCONNECTED;
            if (s_repair_after_disconnect) {
                s_repair_after_disconnect = false;
                s_a2d_state = APP_AV_STATE_DISCOVERING;
                esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
            }'''+s[loc+len('s_a2d_state =  APP_AV_STATE_UNCONNECTED;'):]
# A headless speaker cannot respond to an ESP32 display/keyboard SSP flow.
s=s.replace('esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;',
            'esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;')
# A two-second heartbeat responds to PAIR quickly; keep connecting timeout ~20s.
s=s.replace('(10000 / portTICK_PERIOD_MS)', '(2000 / portTICK_PERIOD_MS)')
s=s.replace('if (++s_connecting_intv >= 2)', 'if (++s_connecting_intv >= 10)')


needle='    bt_app_task_start_up();'
assert needle in s
s=s.replace(needle,'    hw_bridge_start();\n'+needle,1)
# Legacy PIN 0000 (some Victrola revisions require this) rather than Espressif demo PIN 1234.
s=s.replace('Input pin code: 1234','Input pin code: 0000')
s=s.replace("pin_code[0] = '1';\n            pin_code[1] = '2';\n            pin_code[2] = '3';\n            pin_code[3] = '4';", "pin_code[0] = '0';\n            pin_code[1] = '0';\n            pin_code[2] = '0';\n            pin_code[3] = '0';")
main.write_text(s)
cmake=dest/'main/CMakeLists.txt'
cm=cmake.read_text()
# Existing sample builds main.c + bt_app_core.c. Just append bridge.c to SRCS.
assert 'bt_app_core.c' in cm
cm=cm.replace('"bt_app_core.c"','"bt_app_core.c" "bridge.c"')
# Bridge accesses UART and esp_timer directly; IDF 5.x requires explicit component linkage.
assert 'PRIV_REQUIRES bt nvs_flash' in cm
cm=cm.replace('PRIV_REQUIRES bt nvs_flash', 'PRIV_REQUIRES bt nvs_flash esp_driver_uart esp_timer', 1)
cmake.write_text(cm)
# Keep the upstream SDK defaults, especially its Classic-only controller mode.
defaults=dest/'sdkconfig.defaults'
config=defaults.read_text()
if 'CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY=y' not in config:
    sys.exit('Upstream A2DP example no longer requests BR/EDR-only; inspect its Kconfig')
config+='\nCONFIG_EXAMPLE_PEER_DEVICE_NAME="ITSBO-513"\n'
defaults.write_text(config)
print('A2DP source prepared:',dest)
print('IMPORTANT: Build for esp32 (NOT esp32s3); verify chip and backup before flashing.')