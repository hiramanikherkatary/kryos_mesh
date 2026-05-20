#include <stdbool.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_mesh.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "psa/crypto.h"

#include "mesh_config.h"

/*
 * KryOS firmware traceability:
 * - SYS-01/FW-ESP-MESH: standalone ESP-MESH, no router or cloud dependency.
 * - FW-SQC: sensor quorum consensus, not PBFT.
 * - FW-QUORUM: N=4 lab nodes, f=0, all 4 verified non-outlier readings.
 */

#define KRYOS_RX_BUF_SIZE              160
#define KRYOS_PROTOCOL_MAGIC           0x4B59u
#define KRYOS_PROTOCOL_VERSION         1u

typedef enum {
    KRYOS_MSG_SENSOR_READING = 1,
    KRYOS_MSG_SENSOR_FAULT = 2,
    KRYOS_MSG_CONSENSUS = 3,
} kryos_msg_type_t;

typedef enum {
    KRYOS_STATUS_NOMINAL = 0,
    KRYOS_STATUS_OUTLIER_DETECTED = 1,
    KRYOS_STATUS_QUORUM_FAIL = 2,
    KRYOS_STATUS_AUTH_FAIL = 3,
    KRYOS_STATUS_NODE_FAULT = 4,
    KRYOS_STATUS_LINK_FAULT = 5,
} kryos_status_t;

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t node_id;
    uint8_t quality_score;
    int16_t rssi_dbm;
    uint32_t round_id;
    uint32_t timestamp_s;
    int32_t temperature_milli_c;
    uint8_t fault_code;
    uint8_t reserved[3];
} kryos_sensor_frame_t;

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t version;
    uint8_t type;
    uint32_t round_id;
    uint32_t timestamp_s;
    int32_t consensus_milli_c;
    uint8_t node_mask;
    uint8_t rejected_mask;
    uint8_t fault_mask;
    uint8_t verified_count;
    uint8_t quorum_ok;
    uint8_t network_quality;
    uint8_t status;
    uint8_t reserved;
} kryos_consensus_frame_t;

typedef struct {
    bool present;
    bool auth_ok;
    bool sensor_fault;
    uint8_t quality_score;
    uint8_t consecutive_rejections;
    uint32_t last_round_id;
    uint32_t last_seen_s;
    int32_t temperature_milli_c;
    int16_t rssi_dbm;
    uint8_t mac[6];
    bool mac_valid;
} kryos_node_slot_t;

static const char *TAG = LOG_TAG_MESH;
static const uint8_t s_mesh_id[6] = {
    MESH_ID_0, MESH_ID_1, MESH_ID_2, MESH_ID_3, MESH_ID_4, MESH_ID_5
};

static uint8_t s_rx_buf[KRYOS_RX_BUF_SIZE];
static bool s_mesh_connected;
static bool s_mesh_started;
static bool s_tasks_started;
static bool s_recovery_task_started;
static int s_mesh_layer = -1;
static int16_t s_parent_rssi = -50;
static mesh_addr_t s_parent_addr;
static esp_netif_t *s_netif_sta;
static kryos_node_slot_t s_slots[KRYOS_CONSENSUS_NODE_COUNT];
static uint32_t s_local_round_id;
static uint32_t s_consensus_round_id;
static bool s_use_synthetic_temperature;
static float s_synthetic_temperature_c;
static kryos_status_t s_last_status = KRYOS_STATUS_NOMINAL;
static int64_t s_last_mesh_disconnect_us;
static int64_t s_last_rejoin_action_us;
static int64_t s_last_poor_link_reselect_us;
static uint32_t s_mesh_restart_count;
static uint8_t s_consecutive_poor_link_rounds;

static esp_err_t log_esp_err(esp_err_t err, const char *what)
{
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(err));
    }

    return err;
}

static uint32_t now_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

static uint32_t elapsed_ms_since(int64_t since_us)
{
    if (since_us <= 0) {
        return UINT32_MAX;
    }

    return (uint32_t)((esp_timer_get_time() - since_us) / 1000);
}

static float milli_to_c(int32_t milli_c)
{
    return (float)milli_c / 1000.0f;
}

static int32_t c_to_milli(float celsius)
{
    return (int32_t)(celsius * 1000.0f);
}

static uint8_t quality_from_rssi(int rssi)
{
    if (rssi >= ASQC_SIGNAL_EXCELLENT) {
        return 100;
    }
    if (rssi <= ASQC_SIGNAL_POOR) {
        return 0;
    }

    return (uint8_t)(((rssi - ASQC_SIGNAL_POOR) * 100) /
                     (ASQC_SIGNAL_EXCELLENT - ASQC_SIGNAL_POOR));
}

static bool temperature_valid(float temp_c)
{
    return temp_c >= TEMP_MIN_VALID && temp_c <= TEMP_MAX_VALID;
}

static bool link_quality_usable(uint8_t quality_score)
{
    return quality_score >= KRYOS_MIN_LINK_QUALITY;
}

static void update_parent_rssi(void)
{
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_parent_rssi = ap.rssi;
    }
}

static const char *status_name(kryos_status_t status)
{
    switch (status) {
    case KRYOS_STATUS_NOMINAL:
        return "NOMINAL";
    case KRYOS_STATUS_OUTLIER_DETECTED:
        return "OUTLIER_DETECTED";
    case KRYOS_STATUS_QUORUM_FAIL:
        return "QUORUM_FAIL";
    case KRYOS_STATUS_AUTH_FAIL:
        return "AUTH_FAIL";
    case KRYOS_STATUS_NODE_FAULT:
        return "NODE_FAULT";
    case KRYOS_STATUS_LINK_FAULT:
        return "LINK_FAULT";
    default:
        return "UNKNOWN";
    }
}

static void request_parent_reselection(const char *reason)
{
    if (KRYOS_IS_MESH_ROOT || !s_mesh_started) {
        return;
    }

    uint32_t action_age_ms = elapsed_ms_since(s_last_rejoin_action_us);
    uint32_t poor_reselect_age_ms = elapsed_ms_since(s_last_poor_link_reselect_us);
    if (action_age_ms < KRYOS_MESH_REJOIN_BACKOFF_MS ||
        poor_reselect_age_ms < KRYOS_MESH_POOR_LINK_RESELECT_BACKOFF_MS) {
        return;
    }

    ESP_LOGW(TAG, "Forcing parent reselection: %s", reason);
    esp_err_t err = esp_mesh_set_self_organized(true, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Parent reselection request failed: %s", esp_err_to_name(err));
    }

    int64_t now_us = esp_timer_get_time();
    s_last_rejoin_action_us = now_us;
    s_last_poor_link_reselect_us = now_us;
}

static int compare_i32(const void *left, const void *right)
{
    int32_t a = *(const int32_t *)left;
    int32_t b = *(const int32_t *)right;

    return (a > b) - (a < b);
}

static int32_t median_milli(const int32_t *values, uint8_t count)
{
    int32_t copy[KRYOS_CONSENSUS_NODE_COUNT] = {0};

    memcpy(copy, values, count * sizeof(copy[0]));
    qsort(copy, count, sizeof(copy[0]), compare_i32);

    if ((count % 2) == 0) {
        return (copy[count / 2 - 1] + copy[count / 2]) / 2;
    }

    return copy[count / 2];
}

static bool should_send_to_mac(const uint8_t *mac);

static esp_err_t send_to_mesh_root(const void *payload, size_t payload_len)
{
    mesh_data_t data = {
        .data = (uint8_t *)payload,
        .size = payload_len,
        .proto = MESH_PROTO_BIN,
        .tos = MESH_TOS_P2P,
    };

    return esp_mesh_send(NULL, &data, MESH_DATA_TODS, NULL, 0);
}

static esp_err_t send_from_mesh_root(const void *payload, size_t payload_len)
{
    mesh_data_t data = {
        .data = (uint8_t *)payload,
        .size = payload_len,
        .proto = MESH_PROTO_BIN,
        .tos = MESH_TOS_P2P,
    };

    mesh_addr_t route_table[KRYOS_MAX_MESH_DEVICES] = {0};
    int route_table_size = 0;

    esp_mesh_get_routing_table(route_table, sizeof(route_table), &route_table_size);
    for (int i = 0; i < route_table_size; ++i) {
        if (!should_send_to_mac(route_table[i].addr)) {
            continue;
        }
        esp_err_t err = esp_mesh_send(&route_table[i], &data, MESH_DATA_FROMDS, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Consensus fanout to " MACSTR " failed: %s",
                     MAC2STR(route_table[i].addr), esp_err_to_name(err));
        }
    }

    return ESP_OK;
}

static esp_err_t send_mesh_payload(const void *payload, size_t payload_len)
{
    if (KRYOS_IS_MESH_ROOT) {
        return send_from_mesh_root(payload, payload_len);
    }

    return send_to_mesh_root(payload, payload_len);
}

static float read_temperature_sensor_c(bool *fault)
{
    /*
     * Hardware adapter point: replace this simulator with TMP117/MAX31865 IO.
     * Deterministic freezer-range values make the four-node lab consensus
     * easy to verify before sensor bring-up:
     * node 1=4.0 C, node 2=4.2 C, node 3=4.4 C, node 4=4.6 C.
     */
    float temp_c = KRYOS_SIM_TEMP_BASE_C +
                   ((float)(KRYOS_NODE_ID - 1) * KRYOS_SIM_TEMP_STEP_C);

    *fault = !temperature_valid(temp_c);
    return temp_c;
}

static float get_temperature_reading_c(bool *fault)
{
    if (s_use_synthetic_temperature) {
        *fault = !temperature_valid(s_synthetic_temperature_c);
        return s_synthetic_temperature_c;
    }

    return read_temperature_sensor_c(fault);
}

static kryos_node_slot_t *slot_for_node_id(uint8_t node_id)
{
    if (node_id == 0 || node_id > KRYOS_CONSENSUS_NODE_COUNT) {
        return NULL;
    }

    return &s_slots[node_id - 1];
}

static void update_slot_from_reading(uint8_t node_id, uint32_t round_id,
                                     bool sensor_fault, float temp_c,
                                     uint8_t quality_score, int16_t rssi_dbm,
                                     const uint8_t *mac)
{
    kryos_node_slot_t *slot = slot_for_node_id(node_id);
    if (slot == NULL) {
        return;
    }

    slot->present = true;
    slot->auth_ok = true;
    slot->sensor_fault = sensor_fault;
    slot->quality_score = quality_score;
    slot->last_round_id = round_id;
    slot->last_seen_s = now_s();
    slot->temperature_milli_c = c_to_milli(temp_c);
    slot->rssi_dbm = rssi_dbm;
    if (mac != NULL) {
        memcpy(slot->mac, mac, sizeof(slot->mac));
        slot->mac_valid = true;
    }
}

static void mark_slot_offline_by_mac(const uint8_t *mac, const char *reason)
{
    if (mac == NULL) {
        return;
    }

    for (uint8_t i = 0; i < KRYOS_CONSENSUS_NODE_COUNT; ++i) {
        kryos_node_slot_t *slot = &s_slots[i];

        if (!slot->mac_valid || memcmp(slot->mac, mac, sizeof(slot->mac)) != 0) {
            continue;
        }

        slot->present = false;
        slot->sensor_fault = true;
        slot->quality_score = 0;
        slot->consecutive_rejections = 0;
        slot->last_round_id = 0;
        slot->last_seen_s = 0;
        slot->rssi_dbm = 0;

        ESP_LOGW(TAG, "Node %" PRIu8 " offline (%s); dropping from consensus",
                 (uint8_t)(i + 1), reason);
        return;
    }
}

static bool should_send_to_mac(const uint8_t *mac)
{
    if (mac == NULL) {
        return true;
    }

    for (uint8_t i = 0; i < KRYOS_CONSENSUS_NODE_COUNT; ++i) {
        const kryos_node_slot_t *slot = &s_slots[i];

        if (!slot->mac_valid || memcmp(slot->mac, mac, sizeof(slot->mac)) != 0) {
            continue;
        }

        return slot->present;
    }

    return true;
}

static void publish_sensor_frame(bool sensor_fault, float temp_c)
{
    update_parent_rssi();
    uint8_t quality_score = quality_from_rssi(s_parent_rssi);

    kryos_sensor_frame_t frame = {
        .magic = KRYOS_PROTOCOL_MAGIC,
        .version = KRYOS_PROTOCOL_VERSION,
        .type = sensor_fault ? KRYOS_MSG_SENSOR_FAULT : KRYOS_MSG_SENSOR_READING,
        .node_id = KRYOS_NODE_ID,
        .quality_score = quality_score,
        .rssi_dbm = s_parent_rssi,
        .round_id = ++s_local_round_id,
        .timestamp_s = now_s(),
        .temperature_milli_c = c_to_milli(temp_c),
        .fault_code = sensor_fault ? 1 : 0,
    };

    if (!link_quality_usable(frame.quality_score)) {
        if (s_consecutive_poor_link_rounds < UINT8_MAX) {
            s_consecutive_poor_link_rounds++;
        }
        ESP_LOGW(TAG, "ASQC low link quality: RSSI=%d dBm quality=%" PRIu8 "%%",
                 frame.rssi_dbm, frame.quality_score);

        if (s_consecutive_poor_link_rounds >= KRYOS_MESH_POOR_LINK_RESELECT_ROUNDS) {
            request_parent_reselection("link quality below ASQC threshold");
            s_consecutive_poor_link_rounds = 0;
        }
    } else {
        s_consecutive_poor_link_rounds = 0;
    }

    esp_err_t err = send_mesh_payload(&frame, sizeof(frame));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Sensor frame round %" PRIu32 " send failed: %s",
                 frame.round_id, esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Sensor round %" PRIu32 ": %.3f C, q=%" PRIu8 "%%, %s",
             frame.round_id, temp_c, frame.quality_score,
             sensor_fault ? "FAULT" :
             (link_quality_usable(frame.quality_score) ? "OK" : "LOW_LINK"));
}

static void sensor_task(void *arg)
{
    ESP_LOGI(TAG, "Field sensor task started for node %" PRIu8, KRYOS_NODE_ID);

    while (true) {
        if (s_mesh_connected) {
            bool sensor_fault = false;
            float temp_c = get_temperature_reading_c(&sensor_fault);
            publish_sensor_frame(sensor_fault, temp_c);
        }

        vTaskDelay(pdMS_TO_TICKS(KRYOS_SENSOR_PERIOD_MS));
    }
}

static void synthetic_temperature_task(void *arg)
{
    char line[64];

    ESP_LOGI(TAG, "Synthetic temperature input task started. Enter a decimal value in Celsius or 'random' to restore simulation.");

    while (true) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (strncmp(line, "random", 6) == 0) {
            s_use_synthetic_temperature = false;
            ESP_LOGI(TAG, "Synthetic temperature disabled. Reverting to simulated values.");
            continue;
        }

        float temp_c = 0.0f;
        if (sscanf(line, "%f", &temp_c) == 1) {
            s_synthetic_temperature_c = temp_c;
            s_use_synthetic_temperature = true;
            ESP_LOGI(TAG, "Synthetic temperature set to %.3f C", temp_c);
        } else {
            ESP_LOGW(TAG, "Invalid temperature input: %s", line);
        }
    }
}

static void handle_sensor_frame(const kryos_sensor_frame_t *frame, const mesh_addr_t *from)
{
    if (KRYOS_IS_LEADER) {
        if (frame->node_id == 0 || frame->node_id > KRYOS_CONSENSUS_NODE_COUNT) {
            ESP_LOGW(TAG, "Leader ignoring invalid child node=%" PRIu8, frame->node_id);
            return;
        }

        update_slot_from_reading(frame->node_id, frame->round_id,
                     frame->type == KRYOS_MSG_SENSOR_FAULT,
                     milli_to_c(frame->temperature_milli_c),
                     frame->quality_score, frame->rssi_dbm,
                     from->addr);
        ESP_LOGI(TAG, "LEADER RX child=" MACSTR " node=%" PRIu8 " round=%" PRIu32
                      " temp=%.3f C q=%" PRIu8 "%% %s",
                 MAC2STR(from->addr), frame->node_id, frame->round_id,
                 milli_to_c(frame->temperature_milli_c), frame->quality_score,
                 frame->type == KRYOS_MSG_SENSOR_FAULT ? "FAULT" :
                 (link_quality_usable(frame->quality_score) ? "OK" : "LOW_LINK"));
        return;
    }

    if (!KRYOS_IS_ROOT_NODE) {
        return;
    }

    if (frame->node_id == 0 || frame->node_id > KRYOS_CONSENSUS_NODE_COUNT) {
        ESP_LOGW(TAG, "Ignoring invalid sensor frame from node=%" PRIu8, frame->node_id);
        return;
    }

    update_slot_from_reading(frame->node_id, frame->round_id,
                             frame->type == KRYOS_MSG_SENSOR_FAULT,
                             milli_to_c(frame->temperature_milli_c),
                             frame->quality_score, frame->rssi_dbm,
                             from->addr);

    ESP_LOGI(TAG, "RX node=%" PRIu8 " round=%" PRIu32 " temp=%.3f C q=%" PRIu8 "%% %s",
             frame->node_id, frame->round_id, milli_to_c(frame->temperature_milli_c),
             frame->quality_score, frame->type == KRYOS_MSG_SENSOR_FAULT ? "FAULT" :
             (link_quality_usable(frame->quality_score) ? "OK" : "LOW_LINK"));
}

static void handle_consensus_frame(const kryos_consensus_frame_t *frame)
{
    if (frame->magic != KRYOS_PROTOCOL_MAGIC ||
        frame->version != KRYOS_PROTOCOL_VERSION ||
        frame->type != KRYOS_MSG_CONSENSUS) {
        return;
    }

    ESP_LOGI(TAG,
             "CONSENSUS round=%" PRIu32 " temp=%.3f C nodes=0x%02x reject=0x%02x fault=0x%02x quorum=%s status=%s",
             frame->round_id, milli_to_c(frame->consensus_milli_c), frame->node_mask,
             frame->rejected_mask, frame->fault_mask, frame->quorum_ok ? "OK" : "FAIL",
             status_name((kryos_status_t)frame->status));
}

static void mesh_rx_task(void *arg)
{
    mesh_addr_t from = {0};
    mesh_data_t data = {
        .data = s_rx_buf,
        .size = sizeof(s_rx_buf),
    };
    int flag = 0;

    ESP_LOGI(TAG, "Mesh RX task started");

    while (true) {
        data.size = sizeof(s_rx_buf);
        esp_err_t err = esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0);
        if (err != ESP_OK || data.size < 4) {
            ESP_LOGW(TAG, "Mesh receive failed: %s size=%u", esp_err_to_name(err), data.size);
            continue;
        }

        uint8_t type = data.data[3];
        if ((type == KRYOS_MSG_SENSOR_READING || type == KRYOS_MSG_SENSOR_FAULT) &&
            data.size == sizeof(kryos_sensor_frame_t)) {
            handle_sensor_frame((const kryos_sensor_frame_t *)data.data, &from);
        } else if (type == KRYOS_MSG_CONSENSUS &&
                   data.size == sizeof(kryos_consensus_frame_t)) {
            handle_consensus_frame((const kryos_consensus_frame_t *)data.data);
        } else {
            ESP_LOGW(TAG, "Dropped unknown frame type=%" PRIu8 " size=%u flag=%d",
                     type, data.size, flag);
        }
    }
}

static void mesh_recovery_task(void *arg)
{
    ESP_LOGI(TAG, "Mesh recovery watchdog started");

    while (true) {
        if (!KRYOS_IS_MESH_ROOT && s_mesh_started && !s_mesh_connected) {
            if (s_last_mesh_disconnect_us == 0) {
                s_last_mesh_disconnect_us = esp_timer_get_time();
            }

            uint32_t disconnected_ms = elapsed_ms_since(s_last_mesh_disconnect_us);
            uint32_t action_age_ms = elapsed_ms_since(s_last_rejoin_action_us);

            if (disconnected_ms >= KRYOS_MESH_REJOIN_RESTART_MS &&
                action_age_ms >= KRYOS_MESH_REJOIN_BACKOFF_MS) {
                ESP_LOGW(TAG,
                         "Mesh orphaned for %" PRIu32 " ms; restarting mesh stack (count=%" PRIu32 ")",
                         disconnected_ms, ++s_mesh_restart_count);

                esp_err_t err = esp_mesh_stop();
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "esp_mesh_stop during rejoin failed: %s",
                             esp_err_to_name(err));
                } else {
                    s_mesh_started = false;
                }

                vTaskDelay(pdMS_TO_TICKS(1000));

                err = esp_mesh_start();
                if (err == ESP_OK) {
                    s_mesh_started = true;
                    s_last_mesh_disconnect_us = esp_timer_get_time();
                    ESP_LOGI(TAG, "Mesh restart requested; waiting for parent");
                } else {
                    ESP_LOGE(TAG, "esp_mesh_start during rejoin failed: %s",
                             esp_err_to_name(err));
                }

                s_last_rejoin_action_us = esp_timer_get_time();
            } else if (disconnected_ms >= KRYOS_MESH_REJOIN_SELECT_PARENT_MS &&
                       action_age_ms >= KRYOS_MESH_REJOIN_BACKOFF_MS) {
                ESP_LOGW(TAG, "Mesh orphaned for %" PRIu32 " ms", disconnected_ms);
                request_parent_reselection("no parent while mesh is started");
            }
        } else if (s_mesh_connected) {
            s_last_mesh_disconnect_us = 0;
            s_last_rejoin_action_us = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void update_leader_local_slot(void)
{
    if (!KRYOS_IS_LEADER) {
        return;
    }

    bool sensor_fault = false;
    float temp_c = get_temperature_reading_c(&sensor_fault);
    update_slot_from_reading(KRYOS_NODE_ID, ++s_local_round_id, sensor_fault,
                             temp_c, 100, 0, NULL);

    ESP_LOGI(TAG, "LEADER local node=%" PRIu8 " round=%" PRIu32
                  " temp=%.3f C %s",
             KRYOS_NODE_ID, s_local_round_id, temp_c,
             sensor_fault ? "FAULT" : "OK");
}

static void build_consensus_round(kryos_consensus_frame_t *out_frame)
{
    const uint32_t now = now_s();
    int32_t candidates[KRYOS_CONSENSUS_NODE_COUNT] = {0};
    uint8_t candidate_nodes[KRYOS_CONSENSUS_NODE_COUNT] = {0};
    uint8_t candidate_count = 0;
    uint8_t node_mask = 0;
    uint8_t fault_mask = 0;
    uint8_t rejected_mask = 0;
    uint16_t quality_sum = 0;
    uint8_t low_link_mask = 0;

    update_leader_local_slot();

    for (uint8_t i = 0; i < KRYOS_CONSENSUS_NODE_COUNT; ++i) {
        kryos_node_slot_t *slot = &s_slots[i];
        uint8_t bit = (uint8_t)(1u << i);

        if (!slot->present || (now - slot->last_seen_s) > KRYOS_NODE_STALE_SECONDS) {
            fault_mask |= bit;
            continue;
        }

        node_mask |= bit;
        quality_sum += slot->quality_score;

        if (!link_quality_usable(slot->quality_score)) {
            low_link_mask |= bit;
            rejected_mask |= bit;
            fault_mask |= bit;
            continue;
        }

        if (slot->sensor_fault) {
            fault_mask |= bit;
            continue;
        }

        candidates[candidate_count] = slot->temperature_milli_c;
        candidate_nodes[candidate_count] = i;
        candidate_count++;
    }

    int32_t consensus_milli = 0;
    uint8_t verified_count = 0;

    if (candidate_count >= KRYOS_ASQC_MIN_QUORUM) {
        int32_t median = median_milli(candidates, candidate_count);
        int32_t delta = c_to_milli(KRYOS_ASQC_DELTA_T_C);
        int64_t sum = 0;

        for (uint8_t i = 0; i < candidate_count; ++i) {
            int32_t diff = candidates[i] > median ? candidates[i] - median : median - candidates[i];
            uint8_t node_index = candidate_nodes[i];
            uint8_t bit = (uint8_t)(1u << node_index);

            if (diff > delta) {
                rejected_mask |= bit;
                if (s_slots[node_index].consecutive_rejections < UINT8_MAX) {
                    s_slots[node_index].consecutive_rejections++;
                }
                if (s_slots[node_index].consecutive_rejections >= KRYOS_FAULT_LATCH_ROUNDS) {
                    fault_mask |= bit;
                }
                continue;
            }

            s_slots[node_index].consecutive_rejections = 0;
            sum += candidates[i];
            verified_count++;
        }

        if (verified_count >= KRYOS_ASQC_MIN_QUORUM) {
            consensus_milli = (int32_t)(sum / verified_count);
        }
    }

    bool quorum_ok = verified_count >= KRYOS_ASQC_MIN_QUORUM;
    kryos_status_t status = KRYOS_STATUS_NOMINAL;
    if (!quorum_ok) {
        status = KRYOS_STATUS_QUORUM_FAIL;
    } else if (low_link_mask != 0) {
        status = KRYOS_STATUS_LINK_FAULT;
    } else if (fault_mask != 0) {
        status = KRYOS_STATUS_NODE_FAULT;
    } else if (rejected_mask != 0) {
        status = KRYOS_STATUS_OUTLIER_DETECTED;
    }
    s_last_status = status;

    *out_frame = (kryos_consensus_frame_t) {
        .magic = KRYOS_PROTOCOL_MAGIC,
        .version = KRYOS_PROTOCOL_VERSION,
        .type = KRYOS_MSG_CONSENSUS,
        .round_id = ++s_consensus_round_id,
        .timestamp_s = now,
        .consensus_milli_c = consensus_milli,
        .node_mask = node_mask,
        .rejected_mask = rejected_mask,
        .fault_mask = fault_mask,
        .verified_count = verified_count,
        .quorum_ok = quorum_ok ? 1 : 0,
        .network_quality = node_mask ? (uint8_t)(quality_sum / __builtin_popcount(node_mask)) : 0,
        .status = (uint8_t)status,
    };
}

static void log_consensus_node_table(const kryos_consensus_frame_t *frame)
{
    const uint32_t now = now_s();

    for (uint8_t i = 0; i < KRYOS_CONSENSUS_NODE_COUNT; ++i) {
        const kryos_node_slot_t *slot = &s_slots[i];
        uint8_t node_id = (uint8_t)(i + 1);
        uint8_t bit = (uint8_t)(1u << i);
        bool active = (frame->node_mask & bit) != 0;
        bool rejected = (frame->rejected_mask & bit) != 0;
        bool faulted = (frame->fault_mask & bit) != 0;
        int32_t age_s = slot->present ? (int32_t)(now - slot->last_seen_s) : -1;

        ESP_LOGI(TAG,
                 "UART_NODE_STATUS node=%" PRIu8 " role=%s present=%s active=%s temp=%.3f C age_s=%" PRId32 " q=%" PRIu8 "%% rssi=%d fault=%s rejected=%s round=%" PRIu32,
                 node_id, node_id == KRYOS_NODE_ID && KRYOS_IS_LEADER ? "LEADER" : "CHILD",
                 slot->present ? "yes" : "no",
                 active ? "yes" : "no",
                 slot->present ? milli_to_c(slot->temperature_milli_c) : 0.0f,
                 age_s, slot->quality_score, slot->rssi_dbm,
                 (slot->sensor_fault || faulted) ? "yes" : "no",
                 rejected ? "yes" : "no", slot->last_round_id);
    }
}

static void leader_stage_consensus_for_root(const kryos_consensus_frame_t *frame)
{
    ESP_LOGI(TAG,
             "ROOT_UPLINK_STAGED round=%" PRIu32 " ts=%" PRIu32 " temp=%.3f C nodes=0x%02x rejected=0x%02x fault=0x%02x verified=%" PRIu8 " quorum=%s status=%s",
             frame->round_id, frame->timestamp_s, milli_to_c(frame->consensus_milli_c),
             frame->node_mask, frame->rejected_mask, frame->fault_mask,
             frame->verified_count, frame->quorum_ok ? "OK" : "FAIL",
             status_name((kryos_status_t)frame->status));

#if KRYOS_ENABLE_ROOT_UPLINK
    /*
     * Future hardware bridge point: send this frame to the separate root ESP32
     * once that board and its UART/SPI protocol are ready.
     */
    ESP_LOGW(TAG, "Root uplink is enabled but no physical transport is implemented yet");
#else
    ESP_LOGI(TAG, "ROOT_UPLINK disabled; consensus payload is only printed to UART for now");
#endif
}

static void consensus_task(void *arg)
{
    ESP_LOGI(TAG, "Leader ASQC consensus task started");

    while (true) {
        kryos_consensus_frame_t frame = {0};
        build_consensus_round(&frame);

        if (frame.quorum_ok) {
            ESP_LOGI(TAG,
                     "UART_CONSENSUS round=%" PRIu32 " temp=%.3f C verified=%" PRIu8 " mask=0x%02x rejected=0x%02x fault=0x%02x quality=%" PRIu8 "%% status=%s",
                     frame.round_id, milli_to_c(frame.consensus_milli_c),
                     frame.verified_count, frame.node_mask, frame.rejected_mask,
                     frame.fault_mask, frame.network_quality,
                     status_name((kryos_status_t)frame.status));
        } else {
            ESP_LOGE(TAG,
                     "UART_QUORUM_FAIL round=%" PRIu32 " verified=%" PRIu8 " active=0x%02x fault=0x%02x rejected=0x%02x",
                     frame.round_id, frame.verified_count, frame.node_mask,
                     frame.fault_mask, frame.rejected_mask);
        }

        log_consensus_node_table(&frame);
        leader_stage_consensus_for_root(&frame);

        (void)send_from_mesh_root(&frame, sizeof(frame));

        uint32_t delay_ms = (s_last_status == KRYOS_STATUS_NOMINAL) ?
                            KRYOS_CONSENSUS_PERIOD_MS : KRYOS_FAST_CONSENSUS_PERIOD_MS;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

static void start_runtime_tasks(void)
{
    if (s_tasks_started) {
        return;
    }

    s_tasks_started = true;
    xTaskCreate(mesh_rx_task, "kryos_rx", STACK_MESH_TASK, NULL, TASK_PRIORITY_MESH, NULL);

    if (KRYOS_IS_LEADER) {
        xTaskCreate(consensus_task, "kryos_asqc", STACK_CONSENSUS_TASK, NULL,
                    TASK_PRIORITY_CONSENSUS, NULL);
        xTaskCreate(synthetic_temperature_task, "kryos_temp_in", STACK_TEMPERATURE_TASK, NULL,
                    TASK_PRIORITY_TEMP, NULL);
    } else if (KRYOS_IS_FIELD_SENSOR) {
        xTaskCreate(sensor_task, "kryos_sensor", STACK_TEMPERATURE_TASK, NULL,
                    TASK_PRIORITY_TEMP, NULL);
        xTaskCreate(synthetic_temperature_task, "kryos_temp_in", STACK_TEMPERATURE_TASK, NULL,
                    TASK_PRIORITY_TEMP, NULL);
    }
}

static void start_mesh_recovery_task(void)
{
    if (s_recovery_task_started || KRYOS_IS_MESH_ROOT) {
        return;
    }

    s_recovery_task_started = true;
    xTaskCreate(mesh_recovery_task, "kryos_rejoin", STACK_MESH_TASK, NULL,
                TASK_PRIORITY_MESH, NULL);
}

static void mesh_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    mesh_addr_t mesh_id = {0};

    switch (event_id) {
    case MESH_EVENT_STARTED:
        esp_mesh_get_id(&mesh_id);
        s_mesh_started = true;
        s_mesh_layer = esp_mesh_get_layer();
        ESP_LOGI(TAG, "MESH_STARTED id=" MACSTR " role=%s",
                 MAC2STR(mesh_id.addr), KRYOS_IS_ROOT_NODE ? "ROOT_BRIDGE" :
                 (KRYOS_IS_LEADER ? "LEADER" : "FIELD"));
        if (KRYOS_IS_MESH_ROOT) {
            s_mesh_connected = true;
            start_runtime_tasks();
        }
        break;

    case MESH_EVENT_STOPPED:
        s_mesh_started = false;
        s_mesh_connected = false;
        ESP_LOGW(TAG, "MESH_STOPPED");
        break;

    case MESH_EVENT_CHILD_CONNECTED: {
        mesh_event_child_connected_t *child = (mesh_event_child_connected_t *)event_data;
        ESP_LOGI(TAG, "CHILD_CONNECTED aid=%d mac=" MACSTR, child->aid, MAC2STR(child->mac));
        break;
    }

    case MESH_EVENT_CHILD_DISCONNECTED: {
        mesh_event_child_disconnected_t *child = (mesh_event_child_disconnected_t *)event_data;
        ESP_LOGW(TAG, "CHILD_DISCONNECTED aid=%d mac=" MACSTR, child->aid, MAC2STR(child->mac));
        if (KRYOS_IS_LEADER) {
            mark_slot_offline_by_mac(child->mac, "mesh child disconnected");
        }
        break;
    }

    case MESH_EVENT_PARENT_CONNECTED: {
        mesh_event_connected_t *connected = (mesh_event_connected_t *)event_data;
        s_mesh_layer = connected->self_layer;
        memcpy(s_parent_addr.addr, connected->connected.bssid, sizeof(s_parent_addr.addr));
        s_mesh_connected = true;
        s_last_mesh_disconnect_us = 0;
        s_last_rejoin_action_us = 0;
        s_last_poor_link_reselect_us = 0;
        s_consecutive_poor_link_rounds = 0;
        ESP_LOGI(TAG, "PARENT_CONNECTED layer=%d parent=" MACSTR "%s",
                 s_mesh_layer, MAC2STR(s_parent_addr.addr),
                 esp_mesh_is_root() ? " ROOT" : "");
        start_runtime_tasks();
        break;
    }

    case MESH_EVENT_PARENT_DISCONNECTED: {
        mesh_event_disconnected_t *disconnected = (mesh_event_disconnected_t *)event_data;
        s_mesh_connected = KRYOS_IS_MESH_ROOT;
        s_mesh_layer = esp_mesh_get_layer();
        if (!KRYOS_IS_MESH_ROOT) {
            s_last_mesh_disconnect_us = esp_timer_get_time();
        }
        ESP_LOGW(TAG, "PARENT_DISCONNECTED reason=%d", disconnected->reason);
        break;
    }

    case MESH_EVENT_NO_PARENT_FOUND: {
        mesh_event_no_parent_found_t *no_parent = (mesh_event_no_parent_found_t *)event_data;
        if (!KRYOS_IS_MESH_ROOT && s_last_mesh_disconnect_us == 0) {
            s_last_mesh_disconnect_us = esp_timer_get_time();
        }
        ESP_LOGW(TAG, "NO_PARENT_FOUND scan_times=%d", no_parent->scan_times);
        break;
    }

    case MESH_EVENT_LAYER_CHANGE: {
        mesh_event_layer_change_t *layer = (mesh_event_layer_change_t *)event_data;
        s_mesh_layer = layer->new_layer;
        ESP_LOGI(TAG, "LAYER_CHANGE new_layer=%d", s_mesh_layer);
        break;
    }

    case MESH_EVENT_ROUTING_TABLE_ADD:
    case MESH_EVENT_ROUTING_TABLE_REMOVE: {
        mesh_event_routing_table_change_t *rt = (mesh_event_routing_table_change_t *)event_data;
        ESP_LOGI(TAG, "ROUTING_TABLE change=%d new=%d",
                 rt->rt_size_change, rt->rt_size_new);
        break;
    }

    case MESH_EVENT_ROOT_FIXED: {
        mesh_event_root_fixed_t *root_fixed = (mesh_event_root_fixed_t *)event_data;
        ESP_LOGI(TAG, "ROOT_FIXED %s", root_fixed->is_fixed ? "enabled" : "disabled");
        break;
    }

    default:
        ESP_LOGD(TAG, "Unhandled mesh event %" PRId32, event_id);
        break;
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        update_parent_rssi();
    }
}

static void initialize_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static void initialize_mesh(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_mesh_netifs(&s_netif_sta, NULL));

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID,
                                               mesh_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(KRYOS_WIFI_TX_POWER_QDBM));
    ESP_LOGI(TAG, "WiFi TX power capped at %.1f dBm", KRYOS_WIFI_TX_POWER_DBM);

    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_mesh_set_topology(MESH_TOPO_TREE));
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(KRYOS_MESH_MAX_LAYER));
    ESP_ERROR_CHECK(esp_mesh_set_ap_assoc_expire(KRYOS_MESH_AP_ASSOC_EXPIRE_SECONDS));
    ESP_ERROR_CHECK(esp_mesh_set_capacity_num(KRYOS_MAX_MESH_DEVICES));
    ESP_ERROR_CHECK(esp_mesh_set_root_healing_delay(KRYOS_MESH_ROOT_HEALING_DELAY_MS));
    ESP_ERROR_CHECK(esp_mesh_fix_root(true));
    (void)log_esp_err(esp_mesh_set_vote_percentage(1), "esp_mesh_set_vote_percentage");
    (void)log_esp_err(esp_mesh_set_xon_qsize(32), "esp_mesh_set_xon_qsize");
    (void)log_esp_err(esp_mesh_disable_ps(), "esp_mesh_disable_ps");

    if (KRYOS_IS_MESH_ROOT) {
        ESP_ERROR_CHECK(esp_mesh_set_type(MESH_ROOT));
    }

    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    memcpy((uint8_t *)&cfg.mesh_id, s_mesh_id, sizeof(s_mesh_id));
    cfg.channel = MESH_CHANNEL;
    cfg.router.ssid_len = 0;
    cfg.mesh_ap.max_connection = MESH_MAX_CHILDREN;
    cfg.mesh_ap.nonmesh_max_connection = 0;
    memcpy((uint8_t *)&cfg.mesh_ap.password, MESH_SOFTAP_PASSWD,
           strlen(MESH_SOFTAP_PASSWD));

    ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(WIFI_AUTH_WPA2_PSK));
    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
    ESP_ERROR_CHECK(esp_mesh_start());
    s_mesh_started = true;
    start_mesh_recovery_task();
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== KryOS ESP32-C3 ESP-MESH / ASQC Firmware ===");
    ESP_LOGI(TAG, "Role=%s NodeID=%" PRIu8 " ConsensusNodes=%d Children=%d Channel=%d TX=%.1f dBm",
             KRYOS_IS_ROOT_NODE ? "ROOT_BRIDGE" :
             (KRYOS_IS_LEADER ? "LEADER" : "FIELD_SENSOR"),
             KRYOS_NODE_ID, KRYOS_CONSENSUS_NODE_COUNT, KRYOS_CHILD_NODE_COUNT,
             MESH_CHANNEL, KRYOS_WIFI_TX_POWER_DBM);

    initialize_nvs();
    ESP_ERROR_CHECK(psa_crypto_init());
    initialize_mesh();

    ESP_LOGI(TAG, "KryOS mesh initialized in offline-first mode");
}
