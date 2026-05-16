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
#include <esp_timer.h>
#include <string.h>
/* esp_ptp public API and internal wire-format types. Both used in
 * either-medium builds: the public API for ptpd_start_port etc., the
 * internal types for the §12.7 Vendor IE parser. esp_ptp's
 * CMakeLists exposes "." (for ptp.h) and "./include" (for esp_ptp.h)
 * via INCLUDE_DIRS, and esp_ptp is in our PRIV_REQUIRES. */
#include <esp_ptp.h>
#include "ptp.h"

#ifdef CONFIG_ESP_PTP_PORT0_MEDIUM_ETH_HWTS
#include "esp_avb.h"
#include "esp_eth_clock.h"
#include <esp_check.h>
#include <esp_eth.h>
#include <esp_eth_phy_ip101.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_vfs_l2tap.h>
#endif

#if defined(CONFIG_ESP_PTP_PORT0_MEDIUM_WIFI_FTM) ||                               \
    defined(CONFIG_ESP_PTP_PORT1_MEDIUM_WIFI_FTM)
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
#ifdef CONFIG_ESP_PTP_PORT0_MEDIUM_ETH_HWTS

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

#endif /* CONFIG_ESP_PTP_PORT0_MEDIUM_ETH_HWTS */

/* ===========================================================================
 * WIFI medium (PORT0 or PORT1), STA role
 * ===========================================================================
 * Wi-Fi STA associates to the bridge's SoftAP, consumes the bridge's
 * beacon Vendor IE (Path C FollowUpInformation), and runs an FTM
 * initiator burst at the §12.8.2 cadence. The full AVB stack
 * (talker/listener, ATDECC) runs on the Wi-Fi data plane via
 * avb_start() with codec disabled.
 */
#if defined(CONFIG_ESP_PTP_PORT0_MEDIUM_WIFI_FTM) ||                               \
    defined(CONFIG_ESP_PTP_PORT1_MEDIUM_WIFI_FTM)

#define AVB_AP_SSID "ESP-AVB-Bridge"

/* Beacon Vendor IE OUI / sub-types are defined in ptp_rpc_proto.h so
 * the bridge marshaller, bridge coprocessor handler, and this STA
 * parser stay in sync. PTP_VND_IE_OUI* + PTP_VND_IE_OUI_TYPE_FOLLOWUP
 * is the §12.7 IE; PTP_VND_IE_OUI_TYPE_TSF_MAPPING is the Plan-A
 * Scramble-Tools-private (gPTP,TSF) mapping IE. */
#include "ptp_rpc_proto.h"

/* FTM cadence target. IEEE 802.1AS-2020 sets
 * initialLogSyncInterval = -3 → 8 messages/s on Wi-Fi. The ESP-IDF
 * FTM API uses burst_period in 100 ms units (allowed: 0=No pref,
 * 2..100). We use 2 (= 200 ms ≈ 5 Hz) since 1 is below the
 * documented minimum. Each burst runs frm_count FTM frames; allowed
 * values are 0(No pref), 16, 24, 32, 64. */
#define AVB_FTM_BURST_PERIOD_100MS 2
#define AVB_FTM_FRM_COUNT 16

static EventGroupHandle_t s_wifi_events;
#define BIT_STA_CONNECTED BIT0
#define BIT_FTM_REPORT_OK BIT1

static uint8_t s_ap_bssid[6] = {0};
static uint8_t s_ap_channel = 0;

/* Plan A FTM-derived sync state. on_vendor_ie writes both markers as
 * IEs arrive; the WIFI_EVENT_FTM_REPORT success handler reads them
 * with the FTM measurement's t1 to compute GM time at the FTM TX
 * moment, then injects via ptpd_inject_sync_pair. The two IEs are
 * carried in the same beacon and processed back-to-back inside the
 * wifi event task, so the pair is naturally atomic.
 *
 *   s_plan_a_gptp_marker_ns: GM time at bridge marshal moment (from
 *                            §12.7 IE preciseOriginTimestamp).
 *   s_plan_a_tsf_marker_us:  bridge AP TSF µs at coprocessor publish
 *                            moment (from TSF mapping IE).
 *
 * Both must be non-zero before the FTM handler uses the pair. */
static int64_t s_plan_a_gptp_marker_ns = 0;
static int64_t s_plan_a_tsf_marker_us = 0;
static bool s_plan_a_seen_gptp = false;
static bool s_plan_a_seen_tsf = false;

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
      /* Per-entry rtt is picosecond-resolution; the aggregate rtt_est
       * is integer nanoseconds, which truncates to 0 at bench distances
       * where one-way delay is sub-ns. Average the valid per-entry rtt
       * values (zeros are invalid samples filtered by IDF), halve for
       * one-way, then round ps → ns at the ptpd boundary. */
      uint64_t avg_rtt_ps = 0;
      uint8_t valid = 0;
      uint8_t n = r->ftm_report_num_entries;
      if (n > 16) n = 16;
      uint64_t best_t1_ps = 0;
      uint64_t best_t2_ps = 0;
      if (n) {
        wifi_ftm_report_entry_t entries[16];
        if (esp_wifi_ftm_get_report(entries, n) == ESP_OK) {
          uint64_t sum_ps = 0;
          int last_valid = -1;
          for (uint8_t i = 0; i < n; ++i) {
            if (entries[i].rtt) {
              sum_ps += entries[i].rtt;
              valid++;
              last_valid = i;
            }
          }
          if (valid) {
            avg_rtt_ps = sum_ps / valid;
          }
          if (last_valid >= 0) {
            best_t1_ps = entries[last_valid].t1;
            best_t2_ps = entries[last_valid].t2;
          }
        }
      }
      int64_t peer_delay_ns;
      if (avg_rtt_ps) {
        uint64_t one_way_ps = avg_rtt_ps / 2;
        peer_delay_ns = (int64_t)((one_way_ps + 500) / 1000);
      } else {
        /* No per-entry data — fall back to ns-resolution aggregate. */
        peer_delay_ns = (int64_t)r->rtt_est / 2;
      }
      int rc = ptpd_inject_peer_delay(0, peer_delay_ns);

      /* Plan A FTM-derived sync pair. If we have a fresh (gPTP, AP-TSF)
       * mapping from the most recent beacon AND a valid FTM measurement
       * this cycle, convert the bridge-side hardware-timestamped t1
       * (in pSec on bridge TSF) into GM time, then back-project the
       * STA's local clock to the FTM RX moment using t2. Feed the pair
       * to the daemon's pair-injection API which runs the same servo
       * machinery as the wired path. */
      int pair_rc = 0;
      int64_t t1_gPTP_ns = 0;
      int64_t local_at_RX_ns = 0;
      if (s_plan_a_seen_gptp && s_plan_a_seen_tsf && best_t1_ps && best_t2_ps) {
        int64_t t1_us       = (int64_t)(best_t1_ps / 1000000ULL);
        int64_t delta_us    = t1_us - s_plan_a_tsf_marker_us;
        t1_gPTP_ns          = s_plan_a_gptp_marker_ns + delta_us * 1000;

        int64_t t2_us       = (int64_t)(best_t2_ps / 1000000ULL);
        int64_t now_us      = esp_timer_get_time();
        struct timespec swn = {0};
        ptpd_now(&swn);
        int64_t sw_now_ns   = (int64_t)swn.tv_sec * 1000000000LL + swn.tv_nsec;
        local_at_RX_ns      = sw_now_ns - (now_us - t2_us) * 1000;

        pair_rc = ptpd_inject_sync_pair(0, t1_gPTP_ns, local_at_RX_ns);
      }

      static uint32_t s_seen = 0;
      if ((++s_seen % 25) == 1) {
        ESP_LOGI(TAG,
                 "FTM report #%u: peer %02x:%02x:%02x:%02x:%02x:%02x "
                 "RTT_est=%u ns  avg_rtt=%llu ps (%u/%u valid)  "
                 "peer_delay=%lld ns  inject_rc=%d  "
                 "pair_rc=%d t1_gPTP=%lld local_RX=%lld",
                 (unsigned)s_seen, r->peer_mac[0], r->peer_mac[1],
                 r->peer_mac[2], r->peer_mac[3], r->peer_mac[4],
                 r->peer_mac[5], (unsigned)r->rtt_est,
                 (unsigned long long)avg_rtt_ps, valid, n,
                 (long long)peer_delay_ns, rc, pair_rc,
                 (long long)t1_gPTP_ns, (long long)local_at_RX_ns);
      }
      xEventGroupSetBits(s_wifi_events, BIT_FTM_REPORT_OK);
    } else {
      ESP_LOGW(TAG, "FTM session failed: status=%d", r->status);
      /* Diagnostic dump of per-entry t1..t4 on the rare statuses where
       * IDF still populates the report (e.g. NO_VALID_MSMT). Tells us
       * which side is shipping zero/garbage timestamps. Rate-limited. */
      static uint32_t s_fail = 0;
      if (r->ftm_report_num_entries && (++s_fail % 5) == 1) {
        wifi_ftm_report_entry_t entries[16];
        uint8_t n = r->ftm_report_num_entries;
        if (n > 16) n = 16;
        if (esp_wifi_ftm_get_report(entries, n) == ESP_OK) {
          for (uint8_t i = 0; i < n; ++i) {
            const wifi_ftm_report_entry_t *e = &entries[i];
            ESP_LOGW(TAG,
                     "  entry %u: rssi=%d rtt=%u ps t1=%llu t2=%llu "
                     "t3=%llu t4=%llu ppm=%d",
                     i, e->rssi, (unsigned)e->rtt,
                     (unsigned long long)e->t1, (unsigned long long)e->t2,
                     (unsigned long long)e->t3, (unsigned long long)e->t4,
                     e->ppm);
          }
        }
      }
    }
    break;
  }
  default:
    break;
  }
}

/* Vendor IE callback — fires for every Vendor IE in scanned/received
 * beacons and probe responses. Filters to our OUI + Type 0 (§12.7
 * "FollowUpInformation" Vendor IE) and decodes the embedded Follow_Up
 * message.
 *
 * Per IEEE 802.1AS-2020 §12.7, the IE payload is an entire 802.1AS
 * Follow_Up message: PTP common header (34 B, §10.6.2) +
 * preciseOriginTimestamp (10 B, §11.4.4) + FollowUpInformation TLV
 * (32 B, §11.4.4.3). Total = sizeof(ptp_follow_up_s) = 76 B.
 *
 * Today this is a validating decoder — it parses the wire bytes and
 * periodically logs the structured fields. The next step is to feed
 * the parsed Follow_Up into the local ptpd via ptpd_inject_sync(0,
 * payload, 76) so it disciplines the STA's clock; that requires the
 * daemon-side injection path to be wired up (tracked separately). */
static void on_vendor_ie(void *ctx, wifi_vendor_ie_type_t type,
                         const uint8_t sa[6], const vendor_ie_data_t *vnd_ie,
                         int rssi) {
  (void)ctx;
  if (type != WIFI_VND_IE_TYPE_BEACON) {
    return;
  }
  if (vnd_ie->vendor_oui[0] != PTP_VND_IE_OUI0 ||
      vnd_ie->vendor_oui[1] != PTP_VND_IE_OUI1 ||
      vnd_ie->vendor_oui[2] != PTP_VND_IE_OUI2) {
    return;
  }
  /* vnd_ie->length covers OUI(3) + oui_type(1) + payload, so payload
   * size is length - 4. */
  int payload_len = (int)vnd_ie->length - 4;
  const uint8_t *payload = vnd_ie->payload;

  /* Plan A: Scramble Tools (gPTP, AP-TSF) mapping IE. Stores the
   * bridge's wifi MAC TSF µs (LE) captured by the coprocessor at
   * publish time. Paired with the §12.7 preciseOriginTimestamp from
   * the same beacon, the FTM handler uses (gPTP_marker, ap_tsf_marker)
   * + measured t1 to compute GM time at the FTM TX moment. */
  if (vnd_ie->vendor_oui_type == PTP_VND_IE_OUI_TYPE_TSF_MAPPING) {
    if (payload_len < PTP_VND_IE_TSF_MAPPING_PAYLOAD_LEN) {
      return;
    }
    int64_t tsf_us = 0;
    for (int i = 0; i < 8; ++i) {
      tsf_us |= ((int64_t)payload[i]) << (8 * i);
    }
    s_plan_a_tsf_marker_us = tsf_us;
    s_plan_a_seen_tsf = true;
    static uint32_t s_tsf_ie_seen = 0;
    if ((++s_tsf_ie_seen % 25) == 1) {
      ESP_LOGI(TAG, "TSF mapping IE #%u: ap_tsf=%lld us",
               (unsigned)s_tsf_ie_seen, (long long)tsf_us);
    }
    return;
  }

  if (vnd_ie->vendor_oui_type != PTP_VND_IE_OUI_TYPE_FOLLOWUP) {
    return;
  }

  static uint32_t s_seen = 0;
  ++s_seen;

  /* Validate size against an 802.1AS-2020 §12.7 Follow_Up payload. */
  if (payload_len != (int)sizeof(struct ptp_follow_up_s)) {
    if ((s_seen % 50) == 1) {
      ESP_LOGW(TAG,
               "Beacon Vendor IE from %02x:%02x:%02x:%02x:%02x:%02x: "
               "payload %d B (expected %u for §12.7 Follow_Up). Skipped.",
               sa[0], sa[1], sa[2], sa[3], sa[4], sa[5], payload_len,
               (unsigned)sizeof(struct ptp_follow_up_s));
    }
    return;
  }

  const struct ptp_follow_up_s *fu = (const struct ptp_follow_up_s *)payload;
  const struct ptp_header_s *h = &fu->header;

  /* Sanity-check the messagetype nibble (low 4 bits) matches Follow_Up.
   * In the gPTP profile the high nibble carries the gPTP majorSdoId; we
   * mask it off before comparing. */
  if ((h->messagetype & 0x0f) != 0x08 /* PTP_MSGTYPE_FOLLOW_UP */) {
    if ((s_seen % 50) == 1) {
      ESP_LOGW(TAG,
               "Beacon Vendor IE payload messagetype 0x%02x, not Follow_Up; "
               "skipped.",
               h->messagetype);
    }
    return;
  }

  /* Decode and log every Nth beacon. RX timestamp would come from the
   * radio for true §12 timing — esp_wifi_set_vendor_ie_cb doesn't
   * surface it, so we settle for clock_gettime() at callback time
   * (worse precision than FTM HW timestamps; matches our chosen
   * carrier-deviation tradeoff). */
  if ((s_seen % 50) == 1) {
    /* sourcePortIdentity: 8-byte clockIdentity + 2-byte portNumber. */
    const uint8_t *gm = h->sourceidentity;
    uint16_t seq = ((uint16_t)h->sequenceid[0] << 8) | h->sequenceid[1];
    /* correctionField (8 B, scaledNs): high 6 B = ns, low 2 B = 2^-16 ns. */
    int64_t correction_ns = 0;
    for (int i = 0; i < 6; i++) {
      correction_ns = (correction_ns << 8) | h->correction[i];
    }
    /* preciseOriginTimestamp: 6-byte seconds + 4-byte nanoseconds. */
    uint64_t secs = 0;
    for (int i = 0; i < 6; i++) {
      secs = (secs << 8) | fu->origintimestamp[i];
    }
    uint32_t nsecs = ((uint32_t)fu->origintimestamp[6] << 24) |
                     ((uint32_t)fu->origintimestamp[7] << 16) |
                     ((uint32_t)fu->origintimestamp[8] << 8) |
                     (uint32_t)fu->origintimestamp[9];

    ESP_LOGI(TAG,
             "§12.7 Follow_Up @beacon from %02x:%02x:%02x:%02x:%02x:%02x "
             "RSSI=%d seen=%u: GM clockIdentity=%02x:%02x:%02x:%02x:%02x:%02x:"
             "%02x:%02x seqId=%u correction=%lld ns precTS=%llu.%09lu",
             sa[0], sa[1], sa[2], sa[3], sa[4], sa[5], rssi, (unsigned)s_seen,
             gm[0], gm[1], gm[2], gm[3], gm[4], gm[5], gm[6], gm[7],
             (unsigned)seq, (long long)correction_ns, (unsigned long long)secs,
             (unsigned long)nsecs);
  }

  /* Stash the §12.7 preciseOriginTimestamp as gPTP_marker for Plan A
   * BEFORE the dedup check — dedup gates downstream inject_sync (to
   * avoid aliasing the servo's rate estimate) but we want the marker
   * to track the most-recent observed value on every beacon, even
   * duplicates, so the FTM pair injection always uses a current pair. */
  {
    uint64_t secs = 0;
    for (int i = 0; i < 6; i++) {
      secs = (secs << 8) | fu->origintimestamp[i];
    }
    uint32_t nsecs = ((uint32_t)fu->origintimestamp[6] << 24) |
                     ((uint32_t)fu->origintimestamp[7] << 16) |
                     ((uint32_t)fu->origintimestamp[8] << 8) |
                     (uint32_t)fu->origintimestamp[9];
    s_plan_a_gptp_marker_ns =
        (int64_t)(secs * 1000000000ULL) + (int64_t)nsecs;
    s_plan_a_seen_gptp = true;
  }

  /* Deduplicate against the previous-seen IE. Beacons fire every
   * ~100 ms (AP DTIM cadence) but the bridge only re-marshals the IE
   * every Sync interval (125 ms by default). So most beacons re-carry
   * the prior Sync's bytes; reprocessing them aliases the servo's
   * rate estimate. Skip when the preciseOriginTimestamp is unchanged
   * — that's the high-entropy field that always advances on a fresh
   * marshal. */
  static uint8_t s_last_origin_ts[10] = {0};
  if (memcmp(s_last_origin_ts, fu->origintimestamp,
             sizeof(s_last_origin_ts)) == 0) {
    return;
  }
  memcpy(s_last_origin_ts, fu->origintimestamp, sizeof(s_last_origin_ts));

  /* Feed the Follow_Up into ptpd. inject_sync bootstraps the daemon —
   * sets selected_source (so ptpd_inject_sync_pair has the GM identity
   * it gates on) and jumps the SW clock from CLOCK_REALTIME=epoch up
   * to GM time on the first beacon. After that, the FTM-derived pair
   * injection (sub-ms precision) runs alongside this coarse beacon-IE
   * injection. Both push the servo toward the same GM clock; in
   * practice the servo converges to the FTM-precision regime. */
  (void)ptpd_inject_sync(0, payload, (size_t)payload_len);
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
    static bool s_logged_ap_caps = false;
    if (!s_logged_ap_caps) {
      ESP_LOGI(TAG, "AP %02x:%02x:%02x:%02x:%02x:%02x ch=%u "
                    "ftm_responder=%d (advertised in beacon)",
               ap_info.bssid[0], ap_info.bssid[1], ap_info.bssid[2],
               ap_info.bssid[3], ap_info.bssid[4], ap_info.bssid[5],
               ap_info.primary, ap_info.ftm_responder);
      s_logged_ap_caps = true;
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
  /* Start FTM client task. Bursts the AP at ~5 Hz, the report handler
   * feeds RTT/2 into ptpd_inject_peer_delay for the wifi_ftm port's
   * servo. Compensates the static beacon-IE pipeline bias from §12.7
   * Vendor IE rather than from real FTM HW timestamps. */
  xTaskCreate(ftm_client_task, "ftm_client", 4096, NULL, 5, NULL);

  /* Wait for STA association before bringing up AVB so the RX callback that
   * avb_net_init registers (esp_wifi_internal_reg_rxcb) sees a
   * connected interface. eth_handle=NULL signals the wifi data-plane
   * to avb_net_transmit_raw + the senders; eth_interface holds the
   * netif if_key ("WIFI_STA_DEF" is what esp_netif_create_default_wifi_sta
   * uses) so avb_net_init can fetch the MAC via esp_netif_get_mac. */
  ESP_LOGI(TAG, "Waiting for STA association before starting AVB");
  xEventGroupWaitBits(s_wifi_events, BIT_STA_CONNECTED, pdFALSE, pdTRUE,
                      portMAX_DELAY);

  /* Bootstrap ptpd on the wifi_ftm port. The daemon's clockIdentity
   * is sourced from the STA's MAC (read in ptp_port_init_wifi_ftm via
   * esp_wifi_get_mac(WIFI_IF_STA)) — Wi-Fi must already be running,
   * which it is by this point. No L2TAP socket opens; Sync arrives
   * via the §12.7 Vendor IE that on_vendor_ie() (registered above)
   * decodes, peer-delay arrives via FTM and ptpd_inject_peer_delay
   * (still to be wired). Interface label "WIFI_STA_DEF" matches the
   * lwIP netif key the rest of the endpoint uses, but the daemon
   * treats it purely as a label on this medium (no netif open). */
  if (ptpd_start_port(0, "WIFI_STA_DEF", ptp_port_medium_wifi_ftm) < 0) {
    ESP_LOGE(TAG, "ptpd_start_port wifi_ftm failed — wireless clock will "
                  "not lock");
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
  avb_config.eth_handle = NULL;
  avb_config.eth_interface = "WIFI_STA_DEF";
  /* C6 test board's onboard ES8311 (per avbconfig.h comment block).
   * Default codec pins (12/13 = MCLK/BCLK) collide with C6 USB-Serial-
   * JTAG (D+/D-), so we override to the C6 board's actual ES8311
   * wiring. None of these pins overlap with USB on the C6. */
  avb_config.codec_pins.mclk = 19;
  avb_config.codec_pins.bclk = 20;
  avb_config.codec_pins.ws = 22;
  avb_config.codec_pins.dout = 21;
  avb_config.codec_pins.din = 23;
  avb_config.codec_pins.i2c_sda = 8;
  avb_config.codec_pins.i2c_scl = 7;
  avb_config.codec_pins.pa = 6;
  avb_config.codec_pins.pa_reverted = false;

  avb_start(&avb_config);
  ESP_LOGI(TAG, "AVB stack started on wifi port");
}

#endif /* WIFI medium configured */

/* ===========================================================================
 * Main
 * ===========================================================================
 */
void app_main(void) {
#ifdef CONFIG_ESP_PTP_PORT0_MEDIUM_ETH_HWTS
  start_ethernet_endpoint();
#endif
#if defined(CONFIG_ESP_PTP_PORT0_MEDIUM_WIFI_FTM) ||                               \
    defined(CONFIG_ESP_PTP_PORT1_MEDIUM_WIFI_FTM)
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
