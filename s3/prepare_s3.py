#!/usr/bin/env python3
"""Take existing Waveshare-compatible LVGL8 sample and replace its app with Halloween Dial."""
from pathlib import Path
import sys,shutil,os
base=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else Path('lvgl_hello_world').resolve()
if not (base/'main/display_init.cpp').exists():sys.exit('Clone https://github.com/jpowen898/lvgl_hello_world first')
if not (base/'components').exists():sys.exit('Board BSP components missing')
source=Path(__file__).resolve().parent.parent
mp3=Path(os.environ.get('HALLOWEEN_MP3', '/home/layuso/Witches_Sound_cleaned_louder.mp3'))
if not mp3.is_file():sys.exit(f'Missing source audio: {mp3}. Copy it to the build host first.')
if mp3.stat().st_size < 100 or mp3.stat().st_size > 1024*1024:sys.exit('Invalid audio file size')
# Only modify a fresh throwaway clone; preserve original source with backup.
target=base/'main/main.cpp'
if (base/'.halloween-injected').exists():sys.exit('Refusing repeat patch')
# We replace the upstream SquareLine UI entirely. Its generated/fonts and generated/images
# directories are not in git, so IDF 5.5 CMake fails if the unused UI component is built.
ui_cmake=base/'components/ui/CMakeLists.txt'
if ui_cmake.exists(): ui_cmake.rename(ui_cmake.with_name('CMakeLists.txt.disabled'))
shutil.copy(target,target.with_suffix('.cpp.original'))
shutil.copy(source/'s3/main.cpp',target)
shutil.copy(mp3,base/'main/Witches_Sound_cleaned_louder.mp3')
shutil.copy(source/'common/scheduler.h',base/'main/scheduler.h')
shutil.copy(source/'common/scheduler.c',base/'main/scheduler.c')
cmake=base/'main/CMakeLists.txt'
cmake.write_text('''idf_component_register(SRCS "main.cpp" "display_init.cpp" "scheduler.c"
  INCLUDE_DIRS "." REQUIRES fatfs sdmmc nvs_flash esp_timer esp_driver_uart
  esp_driver_gpio esp_driver_spi chmorgan__esp-audio-player)

target_add_binary_data(${COMPONENT_LIB} "Witches_Sound_cleaned_louder.mp3" BINARY)
''')
(base/'main/idf_component.yml').write_text('''dependencies:
  esp_lcd_sh8601: "*"
  lvgl/lvgl: "8.4.0"
  chmorgan/esp-audio-player: "==1.0.7"
  chmorgan/esp-libhelix-mp3: "==1.0.3"
''')
(base/'.halloween-injected').write_text('Waveshare UI example modified. ESP-IDF build NOT yet verified.\n')
print('Patched S3 source at',base)
print('Before any FLASH: back up target S3 and verify the actual chip by esptool.')