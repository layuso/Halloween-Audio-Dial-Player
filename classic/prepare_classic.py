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
s=s.replace(needle,needle+'''\n    if (hw_bridge_take_pair_request()) {
        ESP_LOGI(BT_AV_TAG, "User requested ITSBO-513 rediscovery");
        if (s_a2d_state == APP_AV_STATE_CONNECTED) esp_a2d_source_disconnect(s_peer_bda);
        s_a2d_state = APP_AV_STATE_DISCOVERING;
        esp_bt_gap_cancel_discovery();
        esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
    }
''',1)
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
cmake.write_text(cm)
(dest/'sdkconfig.defaults').write_text('CONFIG_EXAMPLE_PEER_DEVICE_NAME="ITSBO-513"\nCONFIG_BT_ENABLED=y\nCONFIG_BT_CLASSIC_ENABLED=y\nCONFIG_BT_BLUEDROID_ENABLED=y\nCONFIG_BT_A2DP_ENABLE=y\n')
print('A2DP source prepared:',dest)
print('IMPORTANT: Build for esp32 (NOT esp32s3); verify chip and backup before flashing.')