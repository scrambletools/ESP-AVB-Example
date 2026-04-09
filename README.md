# ESP AVB Example Talker & Listener

 An example AVB talker and listener implementation using the esp_avb component for ESP-IDF.

This implementation is based on the following standards:
- IEEE 1722-2016 (AVTP)
- IEEE 1722.1-2021 (ATDECC)
- IEEE 802.1Q-2022 (MSRP, MVRP)
- IEEE 802.1AS-2021 (gPTP, based on ESP-IDF PTPd implementation)

Currently supports:
- AVB talker and listener
- simultaneous input and output stream
- class A streams (125 microsecond latency)
- 24/48 in AAF or AM824
- control via ATDECC controller

Anticipated future support:
- AVB over Wifi (class B streams)
- AVB 'lite' (works with any switch, no bandwidth guarantee)
- AES67

About this example:

This example application can operate as talker and/or listener. It uses the esp_avb component which is currently limited to supporting the Everest ES8311 mono CODEC at 24bit/48kHz PCM audio (in either AAF or AM824/IEC61883-6 stream format). The example is designed to demonstrate the use of the esp_avb component with very little knowledge of AVB needed. Our hope is that it shows how the esp_avb component can be easily dropped into an existing ESP-IDF audio application and provide a simple interface to add AVB connectivity for realtime low-latency audio routing.

CONTROLLER: Currently there is a very simple command line ATDECC controller in the tools directory (it is a python script), for basic connectivity testing. The main application is intended to showcase the talker and listener functionality of AVB, so it requires a controller to make a connection. It has been tested with the Hive AVB controller (https://github.com/christophe-calmejane/hive) as well as the Apple ATDECC controller (built into MacOS).

HARDWARE: It has been tested on the ESP32-P4-ETH board from Waveshare, and should work with any board using both the ESP32-P4 and the ES8311 codec. Other ESP32 SoCs will not operate correctly due to lack of hardware timestamp in the Ethernet MAC, which the ESP32P4 does have. Comptability testing has been performed with the MOTU AVB Switch, the MOTU 8D audio interface and the Apple virtual AVB endpoint device (part of MacOS).

OPEN SOURCE: This example app and the esp_avb component were initially developed by Scramble Tools LLC, and they are provided as open source software for the AVB/Milan developer community under the MIT software license to encourage further development of low cost products and solutions for AVB audio networking.

FEEDBACK: Please provide feedback or pull requests via the Github repository.
