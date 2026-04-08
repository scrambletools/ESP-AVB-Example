/*
 * Copyright 2024-2026 Scramble Tools
 * License: MIT
 *
 * AVB Example Application
 *
 * This application demonstrates the use of the ESP_AVB component
 * to create an AVB talker and/or listener.
 */

#include "esp_avb.h"
#include "esp_eth_clock.h"
#include <driver/gpio.h>
#include <esp_check.h>
#include <esp_eth.h>
#include <esp_eth_phy_ip101.h>
#include <esp_event.h>
#include <esp_intr_alloc.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_vfs_l2tap.h>
#include <ethernet_init.h>
#include <ptpd.h>
#include <sdkconfig.h>
#include <string.h>

/* Example config variables */
#define AVB_SPEAKER_VOLUME CONFIG_EXAMPLE_AVB_SPEAKER_VOLUME
#define AVB_MIC_GAIN CONFIG_EXAMPLE_AVB_MIC_GAIN

typedef struct {
  char name[16];
  TaskHandle_t handle;
} task_info_t;

static const char *TAG = "avb_example";
static struct timespec s_next_time;
static bool s_gpio_level;
esp_eth_handle_t eth_handle;
char avb_interface[10];

/* Initialize Ethernet and netif on all available ports */
void init_ethernet_and_netif(void) {

  ESP_ERROR_CHECK(esp_event_loop_create_default());

  eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
  eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
  eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();

  // Increase DMA buffer size and count
  emac_config.dma_burst_len = ETH_DMA_BURST_LEN_32;
  emac_config.intr_priority = 0; // 0 = use default priority
  mac_config.rx_task_stack_size =
      16384;                    // Room for I2S writes in EMAC RX callback
  mac_config.rx_task_prio = 22; // Above AVB main task (21)
  // Set PHY address (usually 0 or 1, check your hardware)
  phy_config.phy_addr = 1;
  phy_config.reset_gpio_num = 5; // GPIO number for PHY reset

  // Create MAC and PHY instances
  esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
  esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);

  // Install Ethernet driver
  esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
  ESP_ERROR_CHECK(esp_eth_driver_install(&config, &eth_handle));
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

  // attach Ethernet driver to TCP/IP stack
  ESP_ERROR_CHECK(
      esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

  // Set the first interface as the AVB interface
  memcpy(avb_interface, esp_netif_base_config.if_key,
         strlen(esp_netif_base_config.if_key));
  ESP_LOGI(TAG, "AVB interface: %s", avb_interface);

  // Start Ethernet
  ESP_ERROR_CHECK(esp_eth_start(eth_handle));
}

void app_main(void) {
  struct timespec cur_time;

  /* Start Ethernet */
  init_ethernet_and_netif();
  ESP_LOGI(TAG, "Ethernet started");

  /* Start PTP */
  int pid = ptpd_start(avb_interface);

  /* Wait for the clock to be available */
  while (esp_eth_clock_gettime(CLOCK_PTP_SYSTEM, &cur_time) == -1) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  /* Set up the AVB configuration */
  avb_config_s avb_config = AVB_DEFAULT_CONFIG();
#if CONFIG_EXAMPLE_AVB_TALKER
  avb_config.talker = true;
#endif
#if CONFIG_EXAMPLE_AVB_LISTENER
  avb_config.listener = true;
#endif

  /* Set the Ethernet handle in the AVB config */
  avb_config.eth_handle = eth_handle;

  /* Set ethernet to promiscuous mode */
  esp_eth_io_cmd_t cmd = ETH_CMD_S_PROMISCUOUS;
  bool promiscuous = true;
  esp_err_t err = esp_eth_ioctl(eth_handle, cmd, &promiscuous);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set ethernet to promiscuous mode");
    abort();
  }

  /* Set the Ethernet interface in the AVB config */
  avb_config.eth_interface = "ETH_0";

  /* Start AVB*/
  avb_start(&avb_config);

  vTaskDelay(pdMS_TO_TICKS(3000));

  /* After AVB is started, you can set additional codec options */
  // MUST USE EVENT TO MAKE SURE CODEC IS INITIALIZED, THEN USE STATUS TO GET
  // CODEC HANDLE void *codec_handle = avb_get_codec_handle(); if (codec_handle)
  // {
  //     if (avb_config.talker) {
  //         esp_codec_dev_set_in_gain(codec_handle, 30.0);
  //     }
  //     if (avb_config.listener) {
  //         esp_codec_dev_set_out_vol(codec_handle, 60.0);
  //     }
  // }

  /* Register callback function for time pulse indicator LED */
  // esp_eth_clock_register_target_cb(CLOCK_PTP_SYSTEM, ts_callback);

  /* Task handles for memory consumption monitoring */
  static const uint task_monitor_period = 1000;    // wait between checks in ms
  static const uint task_monitor_threshold = 1000; // size of high watermark
  char t0_name[] = "main_task";                    // usually under 16 chars
  char t1_name[] = "AVB";
  char t2_name[] = "PTPD";
  TaskHandle_t t0 = xTaskGetHandle(t0_name);
  TaskHandle_t t1 = xTaskGetHandle(t1_name);
  TaskHandle_t t2 = xTaskGetHandle(t2_name);

  /* Main loop */
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(task_monitor_period));

    struct ptpd_status_s ptp_status;
    // If valid PTP status
    if (ptpd_status(pid, &ptp_status) == 0) {
      // Do something with the PTP status
    }

    ESP_LOGI(TAG, "heartbeat");

    // Check memory consumption of tasks periodically, report any tasks with
    // high watermark under threshold
    if (uxTaskGetStackHighWaterMark(t0) < task_monitor_threshold)
      ESP_LOGI(TAG, "TASK %s high water mark = %d", t0_name,
               uxTaskGetStackHighWaterMark(t0));
    if (t1 && uxTaskGetStackHighWaterMark(t1) < task_monitor_threshold)
      ESP_LOGI(TAG, "TASK %s high water mark = %d", t1_name,
               uxTaskGetStackHighWaterMark(t1));
    if (uxTaskGetStackHighWaterMark(t2) < task_monitor_threshold)
      ESP_LOGI(TAG, "TASK %s high water mark = %d", t2_name,
               uxTaskGetStackHighWaterMark(t2));
  }
}
