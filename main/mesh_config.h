#ifndef MESH_CONFIG_H
#define MESH_CONFIG_H

#include <stdint.h>


#ifndef KRYOS_NODE_ID
#define KRYOS_NODE_ID 4
#endif

#define KRYOS_CONSENSUS_NODE_COUNT 4
#define KRYOS_CHILD_NODE_COUNT   (KRYOS_CONSENSUS_NODE_COUNT - 1)
#define KRYOS_MAX_MESH_DEVICES   KRYOS_CONSENSUS_NODE_COUNT

#ifndef KRYOS_ENABLE_ROOT_UPLINK
#define KRYOS_ENABLE_ROOT_UPLINK 1
#endif

/* Election Stability & Hysteresis */
#define KRYOS_ELECTION_HYSTERESIS_DBM   8   /* Must be 8dB better to switch */
#define KRYOS_LEADERSHIP_LOCK_ROUNDS   10   /* Min rounds to hold leadership */

/* Pre-shared keys for HMAC-SHA256 (32 bytes). 
 * In production, these should be unique and securely provisioned. 
 */
#define KRYOS_NODE_PSK { \
    0x4b, 0x72, 0x79, 0x4f, 0x53, 0x5f, 0x4e, 0x6f, \
    0x64, 0x65, 0x5f, 0x53, 0x65, 0x63, 0x72, 0x65, \
    0x74, 0x5f, 0x4b, 0x65, 0x79, 0x5f, 0x32, 0x30, \
    0x32, 0x36, 0x5f, 0x30, 0x35, 0x5f, 0x32, 0x32  \
}

#define KRYOS_MASTER_PSK { \
    0x4b, 0x72, 0x79, 0x4f, 0x53, 0x5f, 0x4d, 0x61, \
    0x73, 0x74, 0x65, 0x72, 0x5f, 0x53, 0x65, 0x63, \
    0x72, 0x65, 0x74, 0x5f, 0x4b, 0x65, 0x79, 0x5f, \
    0x32, 0x30, 0x32, 0x36, 0x5f, 0x30, 0x35, 0x5f  \
}

/* ESP-MESH: offline-first, fixed-root tree. */
#define MESH_CHANNEL             6
#define MESH_SOFTAP_PASSWD       "KryOSMesh"
#define MESH_MAX_CHILDREN        KRYOS_CHILD_NODE_COUNT
#define KRYOS_MESH_MAX_LAYER     3
#define KRYOS_MESH_AP_ASSOC_EXPIRE_SECONDS 60
#define KRYOS_MESH_ROOT_HEALING_DELAY_MS   10000
#define KRYOS_MESH_REJOIN_SELECT_PARENT_MS 10000
#define KRYOS_MESH_REJOIN_RESTART_MS       30000
#define KRYOS_MESH_REJOIN_BACKOFF_MS        5000
#define KRYOS_MESH_POOR_LINK_RESELECT_ROUNDS 3
#define KRYOS_MESH_POOR_LINK_RESELECT_BACKOFF_MS 15000

/* ESP-IDF uses quarter-dBm units. 34 == 8.5 dBm. */
#define KRYOS_WIFI_TX_POWER_QDBM 34
#define KRYOS_WIFI_TX_POWER_DBM  8.5f

/* Mesh ID: ASCII-ish "KRYOS1". Keep identical on all boards. */
#define MESH_ID_0  0x4b
#define MESH_ID_1  0x52
#define MESH_ID_2  0x59
#define MESH_ID_3  0x4f
#define MESH_ID_4  0x53
#define MESH_ID_5  0x31

/* ASQC timing. Bible baseline is 5 s, falling to 1 s under fault/quorum stress. */
#define KRYOS_SENSOR_PERIOD_MS          5000
#define KRYOS_CONSENSUS_PERIOD_MS       5000
#define KRYOS_FAST_CONSENSUS_PERIOD_MS  1000
#ifndef KRYOS_NODE_STALE_SECONDS
#define KRYOS_NODE_STALE_SECONDS        20
#endif

/* Signal quality mapping and minimum acceptable quorum. */
#define ASQC_SIGNAL_EXCELLENT  -30
#define ASQC_SIGNAL_GOOD       -60
#define ASQC_SIGNAL_FAIR       -80
#define ASQC_SIGNAL_THRESHOLD  -75
#define ASQC_SIGNAL_POOR      -120

#define QUALITY_EXCELLENT       90
#define QUALITY_GOOD            70
#define QUALITY_FAIR            50
#define QUALITY_POOR             0
#define KRYOS_MIN_LINK_QUALITY  20

#ifndef KRYOS_ASQC_MIN_QUORUM
#define KRYOS_ASQC_MIN_QUORUM       3
#endif
#define KRYOS_ASQC_DELTA_T_C        1.50f
#define KRYOS_FAULT_LATCH_ROUNDS    10

#if KRYOS_ASQC_MIN_QUORUM > KRYOS_CONSENSUS_NODE_COUNT
#error "KRYOS_ASQC_MIN_QUORUM cannot exceed KRYOS_CONSENSUS_NODE_COUNT"
#endif

/* Deterministic lab temperatures: node 1=4.0 C, node 2=4.2 C, etc. */
#define KRYOS_SIM_TEMP_BASE_C       4.0f
#define KRYOS_SIM_TEMP_STEP_C       0.2f

/* Medical cold-chain freezer/refrigerator monitoring range accepted by firmware. */
#define TEMP_MIN_VALID  -40.0f
#define TEMP_MAX_VALID   85.0f

/* FreeRTOS sizing for ESP32-C3. */
#define TASK_PRIORITY_TEMP       5
#define TASK_PRIORITY_MESH       6
#define TASK_PRIORITY_CONSENSUS  5

#define STACK_TEMPERATURE_TASK   4096
#define STACK_MESH_TASK          4096
#define STACK_CONSENSUS_TASK     4096

/* Logging tags. */
#define LOG_TAG_MESH   "KryOS_MESH"

#endif /* MESH_CONFIG_H */
