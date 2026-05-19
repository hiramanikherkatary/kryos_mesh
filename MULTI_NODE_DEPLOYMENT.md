# Multi-Node Deployment Guide

## Overview
This guide explains how to deploy the ESP32-C3 mesh network code to 4 different nodes with unique identities.

## Node Identification

Each node needs a unique ID for the network to function properly. Here are the default IDs:

| Node | ID | Position | Role |
|------|----|-----------|----|
| Node 1 | 0x01 | Closest to Router | Root/Parent |
| Node 2 | 0x02 | Child of Node 1 | Relay |
| Node 3 | 0x03 | Child of Node 2 | Relay |
| Node 4 | 0x04 | Child of Node 1 | Relay |

## Deployment Steps

### Step 1: Prepare First Build
```bash
cd ~/Documents/Codes/Project\ KryOS/ESP-IDF/hello_world

# Source ESP-IDF environment
. ~/.espressif/v6.0/esp-idf/export.sh

# Set target
idf.py set-target esp32c3

# Configure
idf.py menuconfig

# Build for first time
idf.py build
```

### Step 2: Flash Node 1 (ID: 0x01)
**Board 1 Configuration:**
- Node ID: 0x01
- Role: Root node (should be close to WiFi router)

```bash
# Connect Node 1 via USB
idf.py -p /dev/ttyUSB0 flash monitor

# Wait for logs showing:
# "Mesh network started"
# "ESP32-C3 Mesh Network with ASQC Protocol"
```

### Step 3: Flash Node 2 (ID: 0x02)
**Modify code for Node 2:**

Edit `main/kryos_consensus_nodes.c` line ~295:
```c
// BEFORE:
xTaskCreate(temperature_task, "temp_task", 2048, (void *)0x01, 5, NULL);

// AFTER (for Node 2):
xTaskCreate(temperature_task, "temp_task", 2048, (void *)0x02, 5, NULL);
```

Or use `mesh_config.h` for cleaner configuration:

Edit `main/mesh_config.h`:
```c
// For Node 2:
#define NODE_ID_THIS_DEVICE   0x02
```

Then rebuild and flash:
```bash
idf.py build
idf.py -p /dev/ttyUSB1 flash monitor
```

### Step 4: Flash Nodes 3 and 4
Repeat Step 3 with:
- Node 3: `(void *)0x03`
- Node 4: `(void *)0x04`

```bash
# Node 3
idf.py -p /dev/ttyUSB2 flash monitor

# Node 4
idf.py -p /dev/ttyUSB3 flash monitor
```

## Automated Multi-Node Deployment (Advanced)

### Using Buildvariant Script
Create a script `build_for_node.sh`:

```bash
#!/bin/bash
# Usage: ./build_for_node.sh <node_id> <port>

NODE_ID=$1
PORT=$2

if [ -z "$NODE_ID" ] || [ -z "$PORT" ]; then
    echo "Usage: $0 <node_id> <port>"
    echo "Example: $0 0x01 /dev/ttyUSB0"
    exit 1
fi

# Replace node ID in code
sed -i "s/(void )0x[0-9A-Fa-f]*,/(void )$NODE_ID,/" main/kryos_consensus_nodes.c

# Build
echo "Building for Node ID: $NODE_ID"
idf.py build

# Flash
echo "Flashing to port: $PORT"
idf.py -p "$PORT" flash

# Restore original (optional)
git checkout main/kryos_consensus_nodes.c

echo "Done! Node $NODE_ID is ready."
```

Usage:
```bash
chmod +x build_for_node.sh
./build_for_node.sh 0x01 /dev/ttyUSB0
./build_for_node.sh 0x02 /dev/ttyUSB1
./build_for_node.sh 0x03 /dev/ttyUSB2
./build_for_node.sh 0x04 /dev/ttyUSB3
```

## Configuration Per Node

### Option 1: Direct Code Modification (Simple)
Edit this line in `app_main()`:
```c
xTaskCreate(temperature_task, "temp_task", 2048, (void *)0x01, 5, NULL);
                                                          ^^^^
                                                    Change this per node
```

### Option 2: Using Header File (Recommended)
Edit `mesh_config.h`:
```c
/* Change this per deployment */
#define NODE_ID_THIS_DEVICE   0x01  // Change to 0x02, 0x03, 0x04
```

Then in code, use:
```c
xTaskCreate(temperature_task, "temp_task", 2048, 
            (void *)NODE_ID_THIS_DEVICE, 5, NULL);
```

### Option 3: Runtime Configuration (Advanced)
Modify `app_main()` to read from NVS or GPIO:
```c
uint8_t node_id = 0x01;  // Default

// Can be changed via:
// - NVS flash storage
// - GPIO pin state
// - UART command
// - Web interface
```

## Verification Checklist

### Per Node
- [ ] Correct Node ID (0x01-0x04)
- [ ] Unique MAC address (automatic)
- [ ] Same MESH_ID (0x77, 0x43...)
- [ ] Same Router credentials
- [ ] Same ASQC thresholds

### Network
- [ ] All 4 nodes powered on
- [ ] WiFi router accessible
- [ ] Nodes within WiFi range
- [ ] Serial monitor shows mesh formation

### Operation
- [ ] Node 1 connects to router first
- [ ] Nodes 2, 3, 4 join mesh network
- [ ] RSSI values visible in logs
- [ ] Temperature input accepted
- [ ] ASQC data transmitted between nodes

## Expected Log Output

### Node 1 (Root)
```
========================================
ESP32-C3 Mesh Network with ASQC Protocol
========================================
I (XXX) KryOS_MESH: Mesh network started
I (XXX) KryOS_MESH: Mesh ID: 77:43:43:49:4e:45
I (XXX) KryOS_MESH: Max nodes supported: 4
I (XXX) KryOS_MESH: MESH_EVENT_PARENT_CONNECTED
I (XXX) KryOS_MESH: Mesh Started. Enter temperature in Serial Terminal:
```

### Nodes 2-4 (Child)
```
I (XXX) KryOS_MESH: Mesh network started
I (XXX) KryOS_MESH: MESH_EVENT_CHILD_CONNECTED, child mac: XX:XX:XX:XX:XX:XX
I (XXX) KryOS_MESH: MESH_EVENT_PARENT_CONNECTED, parent mac: XX:XX:XX:XX:XX:XX
```

### Temperature Transmission
```
I (XXX) KryOS_MESH: ASQC Data transmitted - Temp: 23.50°C, Quality: 85%, RSSI: -60 dBm
I (XXX) KryOS_MESH: ASQC Data received from Node 2: Temp=22.00°C, Quality=78%, RSSI=-65 dBm
```

## Troubleshooting Multi-Node Deployment

### Issue: "Mesh node not found"
**Solution:**
- Verify all nodes have same MESH_ID
- Ensure all nodes are powered on
- Check WiFi router connectivity
- Reduce distance between nodes

### Issue: "Only 1-2 nodes in network"
**Solution:**
- Check node IDs are unique
- Verify mesh channel (should be 6)
- Check signal strength (RSSI > -75 dBm)
- Ensure mesh AP password is 8+ characters

### Issue: "Temperature data not received"
**Solution:**
- Check all nodes are connected (layer value in logs)
- Verify ASQC quality score > 50%
- Check node IDs in received packets
- Monitor RSSI values per node

### Issue: "Cannot flash multiple ports"
**Solution:**
- Use one port at a time
- Create separate build directories per node
- Use CI/CD script for batch deployment
- Use development adapter board with multiple headers

## Performance Optimization

### Memory Usage Per Node
```
Code Memory:     ~500 KB
Data Memory:     ~60 KB
Available RAM:   ~100 KB (of 160 KB total)
```

### Recommended Order of Flashing
1. First: Node 1 (root) - should boot successfully alone
2. Then: Node 2 (layer 2)
3. Then: Node 3 (layer 3)
4. Finally: Node 4 (alternate child)

This ensures proper mesh formation.

## Advanced: Custom Build System

### CMake Variant Support
Modify `CMakeLists.txt` in main folder:
```cmake
# Read NODE_ID from environment
if(NOT DEFINED ENV{NODE_ID})
    set(NODE_ID "0x01")
else()
    set(NODE_ID "$ENV{NODE_ID}")
endif()

# Add as compile definition
target_compile_definitions(${COMPONENT_LIB} PRIVATE NODE_ID=${NODE_ID})
```

Then build with:
```bash
NODE_ID=0x01 idf.py build
NODE_ID=0x02 idf.py build
```

## Quick Reference: Ports & IDs

**If your USB ports are:**
```
/dev/ttyUSB0 → Node 1 (ID: 0x01) - Root
/dev/ttyUSB1 → Node 2 (ID: 0x02) - Child
/dev/ttyUSB2 → Node 3 (ID: 0x03) - Child
/dev/ttyUSB3 → Node 4 (ID: 0x04) - Child
```

**Quick deployment:**
```bash
# Terminal 1: Node 1
idf.py -p /dev/ttyUSB0 flash monitor

# Terminal 2: Node 2 (modify ID first)
idf.py -p /dev/ttyUSB1 flash monitor

# Terminal 3: Node 3 (modify ID first)
idf.py -p /dev/ttyUSB2 flash monitor

# Terminal 4: Node 4 (modify ID first)
idf.py -p /dev/ttyUSB3 flash monitor
```

## Success Criteria

Your deployment is successful when:
- ✅ All 4 nodes boot without errors
- ✅ Node 1 connects to WiFi router (PARENT_CONNECTED log)
- ✅ Nodes 2-4 connect to mesh (PARENT_CONNECTED log)
- ✅ RSSI values shown for all nodes
- ✅ Can input temperature on any node
- ✅ ASQC quality scores > 50%
- ✅ Data received on other nodes' terminals

---

**Deployment Checklist**: Ready ✅
