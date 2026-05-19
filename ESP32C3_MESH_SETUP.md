# ESP32-C3 Mesh Network with ASQC Protocol - Setup Guide

## Overview
This project implements a 4-node ESP32-C3 mesh network with temperature sensing and ASQC (Adaptive Signal Quality Control) protocol for robust communication.

## Features
- **Mesh Network**: Supports up to 4 ESP32-C3 nodes
- **Temperature Sensing**: User input or ADC sensor
- **ASQC Protocol**: Adaptive signal quality assessment
- **Quality Score**: 0-100% based on RSSI (-120 to -30 dBm)
- **Event Handling**: Full mesh lifecycle management
- **Error Handling**: Comprehensive logging and retry mechanisms

## Hardware Requirements
- 4x ESP32-C3 Development Boards
- USB cables for programming
- Optional: Temperature sensor (DHT22, DS18B20, or analog sensor with ADC)

## Software Setup

### 1. Set ESP-IDF Environment
```bash
cd ~/Documents/Codes/Project\ KryOS/ESP-IDF/hello_world

# Source ESP-IDF environment (adjust path as needed)
. ~/.espressif/v6.0/esp-idf/export.sh
```

### 2. Set Target to ESP32-C3
```bash
idf.py set-target esp32c3
```

### 3. Configure Project
```bash
idf.py menuconfig
```

**Recommended Settings:**
- **Component Config → ESP-Mesh:**
  - Enable ESP-Mesh
  - Set max child connections: 3
  - Set parent BSSID: (leave default)
  
- **Component Config → WiFi:**
  - Enable WiFi Support
  - WiFi mode: STA + AP (for mesh)

- **Component Config → Common:**
  - Enable ESP Timer
  - Set task watchdog timeout

### 4. Build Project
```bash
idf.py build
```

### 5. Flash to Device
```bash
# Select port (e.g., /dev/ttyUSB0)
idf.py -p /dev/ttyUSB0 flash

# Monitor output
idf.py -p /dev/ttyUSB0 monitor
```

## Code Configuration

### Network Parameters (in main file)
```c
#define MESH_CHANNEL            6          // WiFi channel
#define MESH_SOFTAP_PASSWD      "MESH_PASS" // Mesh password
#define MESH_MAX_NODES          4          // Total nodes (including this one)
#define ASQC_SIGNAL_THRESHOLD   -75        // Signal quality threshold (dBm)
```

### Router Credentials
Update in `app_main()`:
```c
cfg.router.ssid_len = strlen("Your_Router_SSID");
memcpy(cfg.router.ssid, "Your_Router_SSID", cfg.router.ssid_len);
memcpy(cfg.router.password, "Your_Password", strlen("Your_Password"));
```

### Mesh ID
Currently set to: `0x77, 0x43, 0x43, 0x49, 0x4e, 0x45` (VACCINE)
Change as needed for different mesh networks.

## Operation

### 1. Flash All 4 Nodes
- Program all 4 ESP32-C3 boards with the same firmware
- Each will auto-configure based on network topology

### 2. Monitor Temperature Data
```bash
# From monitoring terminal, enter temperature value
Enter temperature value (e.g., 25.5): 23.5
```

### 3. ASQC Protocol Details
- **Quality Score Calculation:**
  - Excellent (90-100%): RSSI > -40 dBm
  - Good (70-89%): RSSI -40 to -60 dBm
  - Fair (50-69%): RSSI -60 to -80 dBm
  - Poor (0-49%): RSSI < -80 dBm

- **Low Signal Handling:**
  - RSSI < -75 dBm triggers retry
  - Quality score included in packet
  - Warning logged to console

## Data Packet Structure (ASQC)
```c
typedef struct {
    int rssi;               // Signal strength (dBm)
    uint8_t node_id;        // Node identifier
    float temperature;      // Temperature reading
    uint32_t timestamp;     // Unix timestamp
    uint8_t retry_count;    // Retry attempts
    uint8_t quality_score;  // 0-100% quality
} asqc_data_t;
```

## Mesh Topology
```
┌─────────────┐
│   Router    │ (WiFi Gateway)
└──────┬──────┘
       │
       │ (Layer 1)
   ┌───┴───┐
   │ Node1 │ (Root - closest to router)
   └───┬───┘
       │ (Layer 2)
   ┌───┴───┐
   │ Node2 │
   └───┬───┘
       │ (Layer 3)
   ┌───┴───┐
   │ Node3 │
   └───────┘
   
Node4 can connect as sibling or child
```

## Troubleshooting

### Issue: Cannot find esp_log.h
**Solution:**
1. Rebuild: `idf.py clean && idf.py build`
2. Check environment: `. ~/.espressif/v6.0/esp-idf/export.sh`
3. Rebuild IntelliSense: VS Code C++ IntelliSense > Rescan

### Issue: Mesh not connecting
1. Verify all nodes have same MESH_ID
2. Check WiFi router SSID/password
3. Ensure all nodes are within WiFi range
4. Check logs: `idf.py -p /dev/ttyUSB0 monitor`

### Issue: Temperature data not transmitting
1. Wait for "Mesh Started" message
2. Ensure at least 2 nodes are running
3. Check RSSI: Should be > -75 dBm
4. Monitor logs for ASQC warnings

### Issue: Build fails with missing components
```bash
idf.py fullclean
idf.py set-target esp32c3
idf.py menuconfig  # Ensure mesh components enabled
idf.py build
```

## Advanced Features

### Adding Temperature Sensor (ADC)
```c
#include "driver/adc.h"

float read_temperature_from_adc(void) {
    // ADC1_CHANNEL_0 on GPIO2 (ESP32-C3)
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_12);
    int raw = adc1_get_raw(ADC1_CHANNEL_0);
    // Convert raw ADC to temperature (sensor-dependent)
    return (raw / 4095.0) * 3.3 * 100; // Example scaling
}
```

### Adding Persistent Storage (NVS)
```c
nvs_handle_t nvs_handle;
nvs_open("storage", NVS_READWRITE, &nvs_handle);
nvs_set_i32(nvs_handle, "temperature_count", counter);
nvs_commit(nvs_handle);
nvs_close(nvs_handle);
```

## Monitoring & Debugging

### Enable Debug Logging
In `menuconfig`:
- Component Config → Log Output → Default log level: DEBUG

### ASQC Protocol Logs
- "ASQC: Weak signal detected" - Signal below threshold
- "ASQC Data transmitted" - Successful transmission with quality score
- "ASQC Data received from Node" - Received temperature data

## Performance Optimization for ESP32-C3

### Memory Considerations (400KB SRAM)
- Task stack: 2048 bytes
- Keep data structures compact
- Use heap efficiently

### Power Saving (Optional)
```c
// Add to app_main() for power-saving mode
esp_mesh_set_sleep_type(MESH_SLEEP_NORMAL);
esp_sleep_enable_timer_wakeup(60 * 1000000); // 60 seconds
```

## File Structure
```
hello_world/
├── main/
│   ├── CMakeLists.txt          (Updated with mesh components)
│   └── kryos_consensus_nodes.c (Main application)
├── .vscode/
│   └── c_cpp_properties.json   (Include paths configured)
├── CMakeLists.txt
├── sdkconfig                   (Project configuration)
└── ESP32C3_MESH_SETUP.md       (This file)
```

## References
- [ESP-IDF Mesh Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_mesh.html)
- [ESP32-C3 Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-c3_datasheet_en.pdf)
- [ESP-IDF Getting Started](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/get-started/)

## License
Development project for KryOS Consensus Nodes

## Support
For mesh networking issues, check:
1. RSSI values (should be > -75 dBm)
2. Node layer (shown in logs)
3. Child connection count
4. Parent node association status
