# Component for AVB Endpoints

This component provides a basic implementation of an AVB talker and listener.

Current features:

- 1 Talker
- 1 Listener
- run talker and listener simultaneously
- up to 2 channels per stream
- AAF PCM audio
- 24 bit audio
- 48 kHz sample rate

Tested with:

- ESP32-P4 and ES8311 codec
- Waveshare ESP32-P4-ETH board (onboard mic and speaker)
- MOTU AVB switch
- Apple Mac Mini M1 and M4
- Sonnet Thunderbolt AVB Adapter
- Hive AVB Controller

ATDECC support:

Note that ATDECC support is limited to functionality that is required for an AVB talker and listener. No effort has been made to implement functionality that is needed to operate as a controller.

- Discovery
    Entity Available
- Enumeration and Control
  - Descriptors
    - Entity
    - Configuration
    - Audio Unit
    - Stream Input
    - Stream Output
    - AVB Interface
    - Clock Source
    - Memory Object
    - Locale
    - Strings
    - Stream Port Input
    - Stream Port Output
    - Audio Cluster
    - Audio Map
    - Control
    - Clock Domain
  - Commands
    - Acquire Entity
    - Lock Entity
    - Entity Available
    - Controller Available
    - Read Descriptor
    - Get Configuration
    - Set Stream Format
    - Get Stream Info
    - Register Unsolicited Notification
    - Deregister Unsolicited Notification
    - Get Counters
- Connection Management
  - Connect TX
  - Disconnect TX
  - Get TX State
  - Connect RX
  - Disconnect RX
  - Get RX State
  - Get TX Connection
