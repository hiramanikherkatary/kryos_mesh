# KryOS Mesh: Fault-Tolerant Sensor Network

This repository contains the firmware for the **KryOS Sensor Nodes**, part of the Fault-Tolerant Edge Gateway for Medical Cold Chain Monitoring. It implements a self-organizing ESP-MESH network with cryptographically authenticated consensus.

## Core Features

### 1. Authenticated Sensor Quorum Consensus (ASQC)
- **Byzantine Fault Tolerance:** Implements a lightweight outlier rejection algorithm.
- **Strict Quorum:** Requires a minimum of **3 out of 4** verified nodes to reach a `NOMINAL` status.
- **Auto-Elimination:** Mathematical detection and exclusion of malfunctioning or "lying" sensors based on median deviation.

### 2. Centralized "Kingmaker" Election
- **Arbiter-Led:** Nodes act as candidates and wait for explicit leadership instructions from the **Root Node Bridge**.
- **Stability Window:** Includes a 15-second discovery phase to ensure the mesh has settled before election.
- **Monotonic Reporting:** Ensures a single leader is active at any time to provide a clean, sequential telemetry stream.

### 3. End-to-End Security
- **HMAC-SHA256:** Every sensor reading is signed at the source using the `KRYOS_NODE_PSK`.
- **Integrity Verification:** The leader verifies child signatures before inclusion in the consensus round.

## Configuration (`mesh_config.h`)

- `KRYOS_NODE_ID`: Unique ID (1-4) for each physical node.
- `MESH_CHANNEL`: Fixed to 6 for the KryOS environment.
- `KRYOS_WIFI_TX_POWER_DBM`: Capped at **8.5 dBm** for regulatory compliance and interference reduction.

## Build & Flash

```bash
# Set unique ID and flash
KRYOS_NODE_ID=1 idf.py build flash monitor
```

## Logs & Status
- **`NOMINAL`**: Full 3/4 quorum reached, data is verified.
- **`QUORUM_FAIL`**: Fewer than 3 nodes are online or verified.
- **`AUTH_FAIL`**: Node signature mismatch detected.
