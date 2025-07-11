# SPDX-License-Identifier: Apache-2.0

# This file contains boards in Zephyr which have been replaced with a new board
# name.
# This allows the system to automatically change the board while at the same
# time prints a warning to the user, that the board name is deprecated.
#
# To add a board rename, add a line in following format:
# set(<old_board_name>_DEPRECATED <new_board_name>)
#
# When adding board aliases here, remember to add a mention in the corresponding GitHub issue
# tracking the removal of API/options
# https://docs.zephyrproject.org/latest/develop/api/api_lifecycle.html#deprecated,
# so these aliases are eventually removed

set(adafruit_feather_DEPRECATED
    adafruit_feather_nrf52840/nrf52840
)
set(arduino_uno_r4_minima_DEPRECATED
    arduino_uno_r4@minima
)
set(arduino_uno_r4_wifi_DEPRECATED
    arduino_uno_r4@wifi
)
set(esp32c6_devkitc_DEPRECATED
    esp32c6_devkitc/esp32c6/hpcore
)
set(qemu_xtensa_DEPRECATED
    qemu_xtensa/dc233c
)
set(mimx8mp_phyboard_pollux/mimx8ml8/m7_DEPRECATED
    phyboard_pollux/mimx8ml8/m7
)
set(mimx8mm_phyboard_polis/mimx8mm6/m4_DEPRECATED
    phyboard_polis/mimx8mm6/m4
)
set(mimxrt1050_evk_DEPRECATED
    mimxrt1050_evk/mimxrt1052/hyperflash
)
set(mimxrt1060_evk_DEPRECATED
    mimxrt1060_evk/mimxrt1064/hyperflash
)
set(mimxrt1060_evk_DEPRECATED
     mimxrt1060_evk/mimxrt1062/qspi
)
set(mimxrt1060_evkb_DEPRECATED
    mimxrt1060_evk@B/mimxrt1062/qspi
)
set(neorv32_DEPRECATED
    neorv32/neorv32/up5kdemo
)
set(xiao_esp32c6_DEPRECATED
    xiao_esp32c6/esp32c6/hpcore
)
set(esp32_devkitc_wroom/esp32/procpu_DEPRECATED
    esp32_devkitc/esp32/procpu
)
set(esp32_devkitc_wrover/esp32/procpu_DEPRECATED
    esp32_devkitc/esp32/procpu
)
set(esp32_devkitc_wroom/esp32/appcpu_DEPRECATED
    esp32_devkitc/esp32/appcpu
)
set(esp32_devkitc_wrover/esp32/appcpu_DEPRECATED
    esp32_devkitc/esp32/appcpu
)
set(scobc_module1_DEPRECATED
    scobc_a1
)
