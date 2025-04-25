# CMakeLists.txt for esp32_network_manager component

if(CONFIG_WMNGR_ENABLED)
    set(srcs
        "src/wifi_manager.c"
        "src/eth_manager.c"
    )
endif(CONFIG_WMNGR_ENABLED)

# The values of REQUIRES and PRIV_REQUIRES should not depend on any configuration choices (CONFIG_xxx macros). 
# This is because requirements are expanded before configuration is loaded. Other component variables 
# (like include paths or source files) can depend on configuration choices.
set(reqs
    "esp_eth"
    "esp_wifi"
    "wpa_supplicant" # for esp_wps.h
    "nvs_flash"
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ${reqs}
)
