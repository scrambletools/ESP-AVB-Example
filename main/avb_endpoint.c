/*
 * Copyright 2024-2026 Scramble Tools
 * License: MIT
 *
 * AVB Endpoint application for ESP-IDF.
 *
 * Single project that targets multiple endpoint configurations via
 * the esp_ptp per-port Kconfig taxonomy
 * (MEDIUM × HOST_IF × TYPE × WIFI_MODE × LINK_SPEED_MBPS):
 *
 *   - Wired endpoint (default, target esp32p4): PORT0 MEDIUM=ETHERNET,
 *     HOST_IF=EMAC, TYPE=PRIMARY. Full AVB stack on Ethernet.
 *
 *   - Wireless endpoint (target esp32c6): PORT0 MEDIUM=WIFI,
 *     HOST_IF=AHB, TYPE=PRIMARY, WIFI_MODE=STA. Wi-Fi STA associates
 *     to the bridge, consumes beacon-IE FollowUpInformation, initiates
 *     FTM.
 *
 *   - Mixed endpoint (e.g. ETHERNET PRIMARY + WIFI/SDIO STA FAILOVER):
 *     future, both medium blocks fire.
 *
 * Per-medium blocks are gated with #ifdef on the relevant port-medium
 * Kconfig. main/CMakeLists.txt additionally target-gates PRIV_REQUIRES
 * so unused deps (esp_wifi on p4, esp_eth on c6) don't appear in the
 * component graph.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <esp_log.h>
#include <string.h>

#ifdef CONFIG_ESP_PTP_PORT0_MEDIUM_ETHERNET
#include "esp_avb.h"
#include "esp_eth_clock.h"
#include <esp_check.h>
#include <esp_eth.h>
#include <esp_eth_phy_ip101.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_ptp.h>
#include <esp_vfs_l2tap.h>
#endif

#if defined(CONFIG_ESP_PTP_PORT0_MEDIUM_WIFI) ||                               \
    defined(CONFIG_ESP_PTP_PORT1_MEDIUM_WIFI)
#include "esp_avb.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#endif

static const char *TAG = "avb_endpoint";

/* ===========================================================================
 * ETHERNET medium (PORT0)
 * ===========================================================================
 */
#ifdef CONFIG_ESP_PTP_PORT0_MEDIUM_ETHERNET

static esp_eth_handle_t s_eth_handle;
static char s_avb_eth_interface[10];

static void init_ethernet_and_netif(void) {
  /* Default event loop may already exist (e.g. created by esp_ptp at
   * constructor time when a wifi_cp port is in the build).
   * ESP_ERR_INVALID_STATE means "already created", which is fine. */
  esp_err_t loop_r = esp_event_loop_create_default();
  if (loop_r != ESP_OK && loop_r != ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(loop_r);
  }

  eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
  eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
  eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();

  emac_config.dma_burst_len = ETH_DMA_BURST_LEN_32;
  emac_config.intr_priority = 0;
  mac_config.rx_task_stack_size = 16384;
  mac_config.rx_task_prio = 22;
  phy_config.phy_addr = 1;
  phy_config.reset_gpio_num = 5;

  esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
  esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);

  esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
  ESP_ERROR_CHECK(esp_eth_driver_install(&config, &s_eth_handle));
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_vfs_l2tap_intf_register(NULL));

  esp_netif_inherent_config_t esp_netif_base_config =
      ESP_NETIF_INHERENT_DEFAULT_ETH();
  esp_netif_config_t esp_netif_config = {
      .base = &esp_netif_base_config, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH};
  esp_netif_base_config.if_key = "ETH_0";
  esp_netif_base_config.if_desc = "eth0";
  esp_netif_base_config.route_prio = 50;
  esp_netif_t *eth_netif = esp_netif_new(&esp_netif_config);

  ESP_ERROR_CHECK(
      esp_netif_attach(eth_netif, esp_eth_new_netif_glue(s_eth_handle)));

  memcpy(s_avb_eth_interface, esp_netif_base_config.if_key,
         strlen(esp_netif_base_config.if_key));
  ESP_LOGI(TAG, "AVB ethernet interface: %s", s_avb_eth_interface);

  ESP_ERROR_CHECK(esp_eth_start(s_eth_handle));
}

static void start_ethernet_endpoint(void) {
  struct timespec cur_time;

  init_ethernet_and_netif();
  ESP_LOGI(TAG, "Ethernet started");

  ptpd_start(s_avb_eth_interface);

  while (clock_gettime(CLOCK_PTP_SYSTEM, &cur_time) == -1) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  avb_config_s avb_config = AVB_DEFAULT_CONFIG();
  avb_config.entity_name = CONFIG_EXAMPLE_AVB_ENTITY_NAME;
  avb_config.model_id = CONFIG_EXAMPLE_AVB_MODEL_ID;
  avb_config.talker = CONFIG_EXAMPLE_AVB_TALKER;
  avb_config.listener = CONFIG_EXAMPLE_AVB_LISTENER;
  avb_config.default_mic_gain_tenth_db = CONFIG_EXAMPLE_AVB_MIC_GAIN_TENTH_DB;
  avb_config.default_speaker_vol_tenth_db =
      CONFIG_EXAMPLE_AVB_SPEAKER_VOL_TENTH_DB;
  avb_config.atdecc_control = CONFIG_EXAMPLE_AVB_REMOTE_CONTROL;
  avb_config.eth_handle = s_eth_handle;
  avb_config.eth_interface = "ETH_0";

  esp_eth_io_cmd_t cmd = ETH_CMD_S_PROMISCUOUS;
  bool promiscuous = true;
  if (esp_eth_ioctl(s_eth_handle, cmd, &promiscuous) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set ethernet to promiscuous mode");
    abort();
  }

  avb_start(&avb_config);
}

#endif /* CONFIG_ESP_PTP_PORT0_MEDIUM_ETHERNET */

/* ===========================================================================
 * WIFI medium (PORT0 or PORT1), STA role
 * ===========================================================================
 * Wi-Fi STA associates to the bridge's SoftAP, consumes the bridge's
 * beacon Vendor IE (Path C FollowUpInformation), and runs an FTM
 * initiator burst at the §12.8.2 cadence. The full AVB stack
 * (talker/listener, ATDECC) runs on the Wi-Fi data plane via
 * avb_start() with codec disabled.
 */
#if defined(CONFIG_ESP_PTP_PORT0_MEDIUM_WIFI) ||                               \
    defined(CONFIG_ESP_PTP_PORT1_MEDIUM_WIFI)

#define AVB_AP_SSID "ESP-AVB-Bridge"

/* Beacon Vendor IE OUI — must match the bridge's publisher
 * (esp_ptp/ptp_beacon_ie.c). */
#define AVB_VENDOR_OUI0 0x02
#define AVB_VENDOR_OUI1 0x00
#define AVB_VENDOR_OUI2 0x00
#define AVB_VENDOR_OUI_TYPE 0x00 /* §12.7 Table 12-4 = FollowUpInformation */

/* FTM cadence target. IEEE 802.1AS-2020 sets
 * initialLogSyncInterval = -3 → 8 messages/s on Wi-Fi. The ESP-IDF
 * FTM API uses burst_period in 100 ms units (allowed: 0=No pref,
 * 2..100). We use 2 (= 200 ms ≈ 5 Hz) since 1 is below the
 * documented minimum. Each burst runs frm_count FTM frames; allowed
 * values are 0(No pref), 16, 24, 32, 64. */
#define AVB_FTM_BURST_PERIOD_100MS 2
#define AVB_FTM_FRM_COUNT 32

static EventGroupHandle_t s_wifi_events;
#define BIT_STA_CONNECTED BIT0
#define BIT_FTM_REPORT_OK BIT1

static uint8_t s_ap_bssid[6] = {0};
static uint8_t s_ap_channel = 0;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id,
                          void *data) {
  (void)arg;
  (void)base;
  switch (id) {
  case WIFI_EVENT_STA_START:
    ESP_LOGI(TAG, "STA started — connecting to '%s'", AVB_AP_SSID);
    esp_wifi_connect();
    break;
  case WIFI_EVENT_STA_CONNECTED: {
    wifi_event_sta_connected_t *e = (wifi_event_sta_connected_t *)data;
    memcpy(s_ap_bssid, e->bssid, 6);
    s_ap_channel = e->channel;
    ESP_LOGI(TAG, "Associated. BSSID %02x:%02x:%02x:%02x:%02x:%02x channel %u",
             e->bssid[0], e->bssid[1], e->bssid[2], e->bssid[3], e->bssid[4],
             e->bssid[5], e->channel);
    xEventGroupSetBits(s_wifi_events, BIT_STA_CONNECTED);
    break;
  }
  case WIFI_EVENT_STA_DISCONNECTED:
    ESP_LOGW(TAG, "Disconnected — retry");
    xEventGroupClearBits(s_wifi_events, BIT_STA_CONNECTED);
    esp_wifi_connect();
    break;
  case WIFI_EVENT_FTM_REPORT: {
    wifi_event_ftm_report_t *r = (wifi_event_ftm_report_t *)data;
    if (r->status == FTM_STATUS_SUCCESS) {
      ESP_LOGI(TAG,
               "FTM report: peer %02x:%02x:%02x:%02x:%02x:%02x  RTT_est=%u ns "
               " entries=%u",
               r->peer_mac[0], r->peer_mac[1], r->peer_mac[2], r->peer_mac[3],
               r->peer_mac[4], r->peer_mac[5], (unsigned)r->rtt_est,
               (unsigned)r->ftm_report_num_entries);
      xEventGroupSetBits(s_wifi_events, BIT_FTM_REPORT_OK);
    } else {
      ESP_LOGW(TAG, "FTM session failed: status=%d", r->status);
    }
    break;
  }
  default:
    break;
  }
}

/* Vendor IE callback — fires for every Vendor IE in scanned/received
 * beacons and probe responses. Filters to our OUI + Type 0
 * (FollowUpInformation) and logs the size of the
 * payload. */
static void on_vendor_ie(void *ctx, wifi_vendor_ie_type_t type,
                         const uint8_t sa[6], const vendor_ie_data_t *vnd_ie,
                         int rssi) {
  (void)ctx;
  if (type != WIFI_VND_IE_TYPE_BEACON) {
    return;
  }
  if (vnd_ie->vendor_oui[0] != AVB_VENDOR_OUI0 ||
      vnd_ie->vendor_oui[1] != AVB_VENDOR_OUI1 ||
      vnd_ie->vendor_oui[2] != AVB_VENDOR_OUI2 ||
      vnd_ie->vendor_oui_type != AVB_VENDOR_OUI_TYPE) {
    return;
  }
  /* vnd_ie->length covers OUI(3) + type(1) + payload, so payload size
   * is length - 4. */
  int payload_len = (int)vnd_ie->length - 4;
  static uint32_t s_seen = 0;
  if ((++s_seen % 50) == 1) {
    ESP_LOGI(TAG,
             "Beacon Vendor IE detected from %02x:%02x:%02x:%02x:%02x:%02x "
             "(OUI %02x:%02x:%02x type %d, %d-byte payload, RSSI %d) "
             "[seen %u]",
             sa[0], sa[1], sa[2], sa[3], sa[4], sa[5], AVB_VENDOR_OUI0,
             AVB_VENDOR_OUI1, AVB_VENDOR_OUI2, AVB_VENDOR_OUI_TYPE, payload_len,
             rssi, (unsigned)s_seen);
  }
}

static void wifi_sta_init(void) {
  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_ERROR_CHECK(esp_netif_init());
  esp_err_t loop_r = esp_event_loop_create_default();
  if (loop_r != ESP_OK && loop_r != ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(loop_r);
  }
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  /* Power save off — beacon-IE timing precision and FTM accuracy both
   * degrade with PS active. AVB-active STAs run with PS off. */
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

  wifi_config_t sta_cfg = {
      .sta =
          {
              .ssid = AVB_AP_SSID,
              .password = "",
              .threshold.authmode = WIFI_AUTH_OPEN,
          },
  };
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             &on_wifi_event, NULL));

  /* Register Vendor IE RX callback. Fires for any IE in beacons/probe
   * responses; we filter to our OUI inside the handler. */
  ESP_ERROR_CHECK(esp_wifi_set_vendor_ie_cb(on_vendor_ie, NULL));

  ESP_ERROR_CHECK(esp_wifi_start());
}

/* FTM client task — initiates one burst per cadence interval against
 * the associated AP. Per IEEE 802.1AS-2020 §12.1.2, the client drives
 * the FTM exchange and uses the t1..t4 timestamps to compute peer
 * delay. */
static void ftm_client_task(void *arg) {
  (void)arg;
  xEventGroupWaitBits(s_wifi_events, BIT_STA_CONNECTED, pdFALSE, pdTRUE,
                      portMAX_DELAY);
  ESP_LOGI(TAG, "FTM client starting");

  while (true) {
    if ((xEventGroupGetBits(s_wifi_events) & BIT_STA_CONNECTED) == 0) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    wifi_ap_record_t ap_info = {0};
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    wifi_ftm_initiator_cfg_t cfg = {
        .channel = ap_info.primary,
        .frm_count = AVB_FTM_FRM_COUNT,
        .burst_period = AVB_FTM_BURST_PERIOD_100MS,
    };
    memcpy(cfg.resp_mac, ap_info.bssid, 6);
    esp_err_t r = esp_wifi_ftm_initiate_session(&cfg);
    if (r != ESP_OK) {
      ESP_LOGW(TAG, "esp_wifi_ftm_initiate_session: %s", esp_err_to_name(r));
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    xEventGroupWaitBits(s_wifi_events, BIT_FTM_REPORT_OK, pdTRUE, pdFALSE,
                        pdMS_TO_TICKS(2000));
    /* §12.8.2 sets the AS cadence at logSyncInterval=-3 = 8 Hz. The
     * IDF FTM API's burst_period (in 100 ms units) defines burst
     * spacing; we sleep one period between sessions. */
    vTaskDelay(pdMS_TO_TICKS(AVB_FTM_BURST_PERIOD_100MS * 100));
  }
}

static void start_wifi_endpoint(void) {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  ESP_LOGI(TAG, "Wi-Fi STA endpoint boot. MAC %02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  s_wifi_events = xEventGroupCreate();
  wifi_sta_init();
  /* FTM client task disabled to isolate the c6 stability issue.
   * Re-enable once the AVB-stack-on-wifi flow is stable. */
  /* xTaskCreate(ftm_client_task, "ftm_client", 4096, NULL, 5, NULL); */

  /* Wait for STA association before bringing up AVB so the RX callback that
   * avb_net_init registers (esp_wifi_internal_reg_rxcb) sees a
   * connected interface. eth_handle=NULL signals the wifi data-plane
   * to avb_net_transmit_raw + the senders; eth_interface holds the
   * netif if_key ("WIFI_STA_DEF" is what esp_netif_create_default_wifi_sta
   * uses) so avb_net_init can fetch the MAC via esp_netif_get_mac. */
  ESP_LOGI(TAG, "Waiting for STA association before starting AVB");
  xEventGroupWaitBits(s_wifi_events, BIT_STA_CONNECTED, pdFALSE, pdTRUE,
                      portMAX_DELAY);

  avb_config_s avb_config = AVB_DEFAULT_CONFIG();
  avb_config.entity_name = CONFIG_EXAMPLE_AVB_ENTITY_NAME;
  avb_config.model_id = CONFIG_EXAMPLE_AVB_MODEL_ID;
  avb_config.talker = CONFIG_EXAMPLE_AVB_TALKER;
  avb_config.listener = CONFIG_EXAMPLE_AVB_LISTENER;
  avb_config.default_mic_gain_tenth_db = CONFIG_EXAMPLE_AVB_MIC_GAIN_TENTH_DB;
  avb_config.default_speaker_vol_tenth_db =
      CONFIG_EXAMPLE_AVB_SPEAKER_VOL_TENTH_DB;
  avb_config.atdecc_control = CONFIG_EXAMPLE_AVB_REMOTE_CONTROL;
  avb_config.eth_handle = NULL;
  avb_config.eth_interface = "WIFI_STA_DEF";
  /* c6 endpoint has no codec attached and the default codec pins
   * (12/13) collide with the USB-serial peripheral. Skip I2S +
   * codec init entirely, and zero the PA pin so identify-tone paths
   * don't try to drive a nonexistent GPIO. */
  avb_config.codec_disabled = true;
  avb_config.codec_pins.pa = -1;

  avb_start(&avb_config);
  ESP_LOGI(TAG, "AVB stack started on wifi port");
}

#endif /* WIFI medium configured */

/* ===========================================================================
 * Main
 * ===========================================================================
 */
void app_main(void) {
#ifdef CONFIG_ESP_PTP_PORT0_MEDIUM_ETHERNET
  start_ethernet_endpoint();
#endif
#if defined(CONFIG_ESP_PTP_PORT0_MEDIUM_WIFI) ||                               \
    defined(CONFIG_ESP_PTP_PORT1_MEDIUM_WIFI)
  start_wifi_endpoint();
#endif

  vTaskDelay(pdMS_TO_TICKS(3000));

  /* Task handles for memory consumption monitoring */
  static const unsigned int task_monitor_period = 1000;
  static const unsigned int task_monitor_threshold = 1000;
  char t0_name[] = "main_task";
  char t1_name[] = "AVB";
  char t2_name[] = "PTPD";
  TaskHandle_t t0 = xTaskGetHandle(t0_name);
  TaskHandle_t t1 = xTaskGetHandle(t1_name);
  TaskHandle_t t2 = xTaskGetHandle(t2_name);

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(task_monitor_period));
    ESP_LOGI(TAG, "heartbeat");

    if (t0 && uxTaskGetStackHighWaterMark(t0) < task_monitor_threshold)
      ESP_LOGI(TAG, "TASK %s high water mark = %d", t0_name,
               uxTaskGetStackHighWaterMark(t0));
    if (t1 && uxTaskGetStackHighWaterMark(t1) < task_monitor_threshold)
      ESP_LOGI(TAG, "TASK %s high water mark = %d", t1_name,
               uxTaskGetStackHighWaterMark(t1));
    if (t2 && uxTaskGetStackHighWaterMark(t2) < task_monitor_threshold)
      ESP_LOGI(TAG, "TASK %s high water mark = %d", t2_name,
               uxTaskGetStackHighWaterMark(t2));
  }
}
