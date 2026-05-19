# KryOS ESP32-C3 Mesh Consensus Firmware

KryOS is an offline-first ESP32-C3 mesh firmware prototype for distributed temperature consensus. It uses ESP-IDF, FreeRTOS, and ESP-MESH so the boards can form their own local network without a Wi-Fi router, internet connection, cloud service, or phone hotspot.

The current lab build runs a 4-node cluster:

| Node | Role | Simulated temperature | Responsibility |
| ---: | --- | ---: | --- |
| 1 | Leader | `4.0 C` | Fixed ESP-MESH root for the cluster, runs consensus, prints UART output |
| 2 | Child sensor | `4.2 C` | Sends temperature readings to the leader |
| 3 | Child sensor | `4.4 C` | Sends temperature readings to the leader |
| 4 | Child sensor | `4.6 C` | Sends temperature readings to the leader |

With all four nodes online, the expected consensus value is:

```text
(4.0 + 4.2 + 4.4 + 4.6) / 4 = 4.3 C
```

## Features

- ESP32-C3 target using ESP-IDF CMake build flow.
- Standalone ESP-MESH tree topology.
- No router dependency: `cfg.router.ssid_len = 0`.
- One leader and three child sensor nodes.
- Sensor Quorum Consensus over the latest reading from all four nodes.
- UART consensus logs with per-node status.
- Future root-ESP32 uplink stub through `ROOT_UPLINK_STAGED`.
- Wi-Fi TX power capped to `8.5 dBm` on all ESP32-C3 boards.
- Deterministic simulated temperatures until physical sensors are connected.

## Repository Layout

```text
.
├── CMakeLists.txt
├── README.md
├── KRYOS_MESH_ARCHITECTURE.md
├── main
│   ├── CMakeLists.txt
│   ├── kryos_consensus_nodes.c
│   └── mesh_config.h
└── sdkconfig
```

See [KRYOS_MESH_ARCHITECTURE.md](KRYOS_MESH_ARCHITECTURE.md) for the detailed protocol and architecture notes.

## Requirements

- 4 ESP32-C3 boards.
- ESP-IDF installed and exported.
- USB serial access to each board.

This project has been built with ESP-IDF v6.0.

## Build Setup

Source ESP-IDF and set the target:

```bash
source /home/hiroroo/.espressif/v6.0/esp-idf/export.sh
idf.py set-target esp32c3
```

## Flash The Leader

Build node 1 as the leader:

```bash
KRYOS_NODE_ROLE_LEADER=1 KRYOS_NODE_ID=1 idf.py reconfigure build
idf.py -p /dev/ttyUSB0 flash monitor
```

The leader is the fixed ESP-MESH root for the current lab cluster. It receives child frames, runs consensus, prints `UART_CONSENSUS`, and stages the future root-uplink payload.

## Flash Child Nodes

Build and flash each child with its own node ID:

```bash
KRYOS_NODE_ROLE_LEADER=0 KRYOS_NODE_ID=2 idf.py reconfigure build
idf.py -p /dev/ttyUSB1 flash monitor

KRYOS_NODE_ROLE_LEADER=0 KRYOS_NODE_ID=3 idf.py reconfigure build
idf.py -p /dev/ttyUSB2 flash monitor

KRYOS_NODE_ROLE_LEADER=0 KRYOS_NODE_ID=4 idf.py reconfigure build
idf.py -p /dev/ttyUSB3 flash monitor
```

Use separate build directories if you want to keep all four firmware images at the same time. If you reuse one `build/` directory, always run `idf.py reconfigure build` after changing node role or node ID.

## Expected UART Output

When all nodes are online, the leader should print lines like:

```text
I (...) KryOS_MESH: UART_CONSENSUS round=97 temp=4.300 C verified=4 mask=0x0f rejected=0x00 fault=0x00 quality=74% status=NOMINAL
I (...) KryOS_MESH: UART_NODE_STATUS node=1 role=LEADER present=yes active=yes temp=4.000 C age_s=0 q=100% rssi=0 fault=no rejected=no round=97
I (...) KryOS_MESH: UART_NODE_STATUS node=2 role=CHILD present=yes active=yes temp=4.200 C age_s=4 q=62% rssi=-64 fault=no rejected=no round=7
I (...) KryOS_MESH: UART_NODE_STATUS node=3 role=CHILD present=yes active=yes temp=4.400 C age_s=6 q=68% rssi=-58 fault=no rejected=no round=20
I (...) KryOS_MESH: UART_NODE_STATUS node=4 role=CHILD present=yes active=yes temp=4.600 C age_s=2 q=68% rssi=-58 fault=no rejected=no round=8
I (...) KryOS_MESH: ROOT_UPLINK_STAGED round=97 ts=201 temp=4.300 C nodes=0x0f rejected=0x00 fault=0x00 verified=4 quorum=OK status=NOMINAL
```

Important fields:

- `temp=4.300 C`: consensus value.
- `verified=4`: all four readings were accepted.
- `mask=0x0f`: nodes 1, 2, 3, and 4 are active.
- `rejected=0x00`: no outliers.
- `fault=0x00`: no stale or faulted nodes.
- `status=NOMINAL`: consensus is healthy.

## Configuration

Main tunables live in [main/mesh_config.h](main/mesh_config.h):

| Setting | Meaning |
| --- | --- |
| `KRYOS_CONSENSUS_NODE_COUNT` | Number of nodes participating in consensus |
| `KRYOS_ASQC_MIN_QUORUM` | Minimum verified readings required for consensus |
| `KRYOS_ASQC_DELTA_T_C` | Maximum allowed distance from median before rejection |
| `KRYOS_SENSOR_PERIOD_MS` | Child sensor publish period |
| `KRYOS_CONSENSUS_PERIOD_MS` | Leader consensus period |
| `KRYOS_WIFI_TX_POWER_QDBM` | TX power in quarter-dBm units; `34` means `8.5 dBm` |
| `KRYOS_ENABLE_ROOT_UPLINK` | Reserved for later leader-to-root ESP32 transport |

## Sensor Bring-Up

Physical temperature sensors are not required for the current lab test. The firmware currently uses deterministic simulated values in `read_temperature_sensor_c()`.

When TMP117, MAX31865, or another sensor is attached, replace only that adapter function and keep the same behavior:

- Return temperature in Celsius.
- Set `*fault = true` if the read fails.
- Set `*fault = true` if the value is outside `TEMP_MIN_VALID..TEMP_MAX_VALID`.

## Current Status

Working:

- 4-node ESP-MESH formation.
- Leader receives child readings.
- Leader computes `4.300 C` consensus.
- UART node status and consensus logging.
- Root-uplink payload staging.
- 8.5 dBm Wi-Fi TX power cap.

Not yet implemented:

- Physical temperature sensor driver.
- Real leader-to-root ESP32 transport.
- Production provisioning/security hardening.
- Larger app partition. The current firmware fits, but the default 1 MB app partition is nearly full.

## Useful Commands

Monitor an already flashed board:

```bash
idf.py -p /dev/ttyUSB0 monitor
```

Build only:

```bash
idf.py build
```

Clean generated build files:

```bash
idf.py fullclean
```

