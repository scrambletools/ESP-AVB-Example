# ESP-AVB-Example Project Notes

## Network Topology
- **MOTU AVB Switch** — central switch, all devices connected to it
- **ESP1** (MAC 80:f1:b2:d1:b8:05) — no tap, no serial access from host
- **ESP2** (MAC 80:f1:b2:d1:f7:f2) — tapped, serial on /dev/ttyACM0
- **MOTU 8D** (MAC 00:01:f2:01:62:e5) — AVB audio endpoint, tapped
- **Mac mini** (MAC d0:11:e5:97:f5:44) — AVB controller + talker/listener
- **Host** — connected to switch via enp5s0f3u4 (for controller.py)

## Tap Interfaces
- **eno1** — switch -> ESP2 (downstream, sees traffic arriving at ESP2)
- **enp4s0** — ESP2 -> switch (upstream, sees traffic leaving ESP2)
- **enp5s0f3u3u1c2 / enp5s0f3u3u3c2** — taps between MOTU 8D and switch
- **enp5s0f3u4** — direct to switch, used for avb_controller.py

## Controller
- `sudo python3 /home/user/Development/avb_tools/avb_controller.py --interface enp5s0f3u4 <command>`
- Entity IDs: ESP1=80:f1:b2:d1:b8:05:00:00, ESP2=80:f1:b2:d1:f7:f2:00:00, Mac=d1:11:e5:97:f5:44:80:00

## Stream Test Results (ESP-to-ESP static bug)
| Talker | Listener | Result |
|--------|----------|--------|
| ESP (mic) | Mac | Clean audio |
| Mac (mic) | ESP | Clean audio |
| ESP (sine, loud) | ESP | Clean tone |
| ESP (sine, quiet ~20k) | ESP | Clean tone |
| ESP (mic) | ESP | **Shrill static** |
| DRAIN_SINE_TEST (bypass network) | ESP local | Clean tone |

## Ruled Out Theories for static in ESP-to-ESP case
- **Stream ID mixing / MAAP conflict** — stream_id filter added, no id_skip in logs
- **Ring buffer partial writes** — changed to all-or-nothing, zero rfail in logs
- **AVTP format mismatch** — wire captures show Mac and ESP packets are byte-for-byte structurally identical (same headers, same 125us timing, same 234-byte frames)
- **AVTP-to-I2S conversion bug** — mathematically verified correct from diagnostics (avtp_audio bytes reverse correctly to i2s_out bytes)
- **Ring buffer / StreamBuffer corruption** — replaced custom ring with FreeRTOS StreamBuffer, same result
- **L1 cache coherency** — added esp_cache_msync flush/invalidate, same result; also tried MALLOC_CAP_DMA allocation
- **Direct I2S write (bypass buffer)** — wrote from EMAC handler directly to i2s_channel_write, same static
- **I2S byte order** — DRAIN_SINE_TEST with [LSB,MID,MSB] plays clean, confirming little-endian is correct
- **Signal amplitude** — quiet sine at mic-level amplitude (~20k) plays clean
- **Packet timing** — both Mac and ESP talkers have identical 125us intervals, no bursts
- **I2S full-duplex contention** — works fine when ESP runs talker+listener simultaneously with Mac
- **Overlapping streams** — wire capture shows 100% AAF subtype in VLAN frames, zero IEC 61883 mixing

## Key Unsolved Observation
Correct I2S data (verified byte-by-byte) goes into i2s_channel_write yet produces static. The SAME code path plays Mac audio and sine waves cleanly. Only ESP mic audio produces static, despite identical wire format and correct conversion.

## Build Notes
- ESP-IDF v6.x, target esp32p4
- ESP2 on /dev/ttyACM0 is chip rev v1.3 (set via menuconfig, don't clean build)
- esp_avb component at /home/user/Development/esp_avb (also at components/esp_avb via submodule)
