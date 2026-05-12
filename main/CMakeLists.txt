set(srcs "avb_endpoint.c")

# Per-target dep gating. The per-medium blocks inside avb_endpoint.c
# self-gate on CONFIG_ESP_PTP_PORT*_MEDIUM_* via #ifdef, but the
# component graph still needs to know which managed/built-in components
# to pull in. esp_wifi only exists on Wi-Fi-capable targets.
#
# For the p4 wired build we deliberately don't pass PRIV_REQUIRES so
# IDF's legacy #include-scanning auto-resolves esp_eth, esp_netif,
# esp_avb, etc. (the historical behavior of this example).
if(IDF_TARGET STREQUAL "esp32c6")
    idf_component_register(SRCS ${srcs}
                           PRIV_REQUIRES esp_wifi esp_netif esp_event
                                         nvs_flash esp_timer esp_avb)
else()
    idf_component_register(SRCS ${srcs})
endif()
