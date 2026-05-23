/*
 * Copyright 2026 Scramble Tools
 * License: MIT
 *
 * Wireless endpoint perf-test mode. When CONFIG_AVB_ENDPOINT_PERF_TEST_MODE
 * is set, app_main calls avb_endpoint_perf_test_run() instead of the
 * production bring-up. Only NVS + esp_event + WiFi STA come up; ptpd,
 * AVB and the codec are NOT started. An esp_wifi_internal_reg_rxcb hook
 * counts AVTP frames whose dest MAC + stream ID match the configured
 * test pair, so the path under measurement (bridge -> SDIO -> 802.11 ->
 * this STA) can be characterised in isolation.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void avb_endpoint_perf_test_run(void);

#ifdef __cplusplus
}
#endif
