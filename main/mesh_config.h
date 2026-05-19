#ifndef MESH_CONFIG_H
#define MESH_CONFIG_H

#include <stdint.h>

/* KryOS ESP32-C3 ESP-MESH configuration.
 *
 * Default build is for a child sensor node.
 *
 * Build examples:
 *   KRYOS_NODE_ROLE_LEADER=1 KRYOS_NODE_ID=1 idf.py build
 *   KRYOS_NODE_ROLE_LEADER=0 KRYOS_NODE_ID=2 idf.py build
 *   KRYOS_NODE_ROLE_LEADER=0 KRYOS_NODE_ID=3 idf.py build
 *   KRYOS_NODE_ROLE_LEADER=0 KRYOS_NODE_ID=4 idf.py build
 */

#ifndef KRYOS_NODE_ID
#define KRYOS_NODE_ID 4
#endif

#ifndef KRYOS_NODE_ROLE_ROOT
#define KRYOS_NODE_ROLE_ROOT 0
#endif

#ifndef KRYOS_NODE_ROLE_LEADER
#define KRYOS_NODE_ROLE_LEADER 0
#endif

#define KRYOS_IS_ROOT_NODE       (KRYOS_NODE_ROLE_ROOT == 1)
#define KRYOS_IS_LEADER          (KRYOS_NODE_ROLE_LEADER == 1)
#define KRYOS_IS_FIELD_SENSOR    (!KRYOS_IS_ROOT_NODE && !KRYOS_IS_LEADER)
#define KRYOS_IS_MESH_ROOT       (KRYOS_IS_ROOT_NODE || KRYOS_IS_LEADER)
#define KRYOS_CONSENSUS_NODE_COUNT 4
#define KRYOS_CHILD_NODE_COUNT   (KRYOS_CONSENSUS_NODE_COUNT - 1)
#define KRYOS_MAX_MESH_DEVICES   KRYOS_CONSENSUS_NODE_COUNT
#define KRYOS_SINGLE_BOARD_LAB   0

#ifndef KRYOS_ENABLE_ROOT_UPLINK
#define KRYOS_ENABLE_ROOT_UPLINK 0
#endif

/* ESP-MESH: offline-first, fixed-root tree. */
#define MESH_CHANNEL             6
#define MESH_SOFTAP_PASSWD       "KryOSMesh"
#define MESH_MAX_CHILDREN        KRYOS_CHILD_NODE_COUNT
#define KRYOS_MESH_MAX_LAYER     3

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
#define KRYOS_NODE_STALE_SECONDS        12

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

#define KRYOS_ASQC_MIN_QUORUM       4
#define KRYOS_ASQC_DELTA_T_C        1.50f
#define KRYOS_FAULT_LATCH_ROUNDS    10

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
