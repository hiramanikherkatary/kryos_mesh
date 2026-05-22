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

#define KRYOS_RX_BUF_SIZE              256
#define KRYOS_PROTOCOL_MAGIC           0x4B59u
#define KRYOS_PROTOCOL_VERSION         1u

typedef enum {
    KRYOS_MSG_SENSOR_READING = 1,
    KRYOS_MSG_SENSOR_FAULT = 2,
    KRYOS_MSG_CONSENSUS = 3,
    KRYOS_MSG_LEADER_ELECT = 4,
    KRYOS_MSG_LEADER_SELECT = 5,
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
    int16_t rssi_dbm;
    uint8_t mac[6];
} kryos_election_frame_t;

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t selected_node_id;
    uint8_t reserved[3];
} kryos_select_frame_t;

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
    uint8_t hmac[32];
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
    uint8_t hmac[32];
} kryos_consensus_frame_t;

typedef struct {
    bool present;
    bool sensor_fault;
    uint8_t quality_score;
    uint32_t last_seen_s;
    int32_t temperature_milli_c;
} kryos_node_slot_t;

static const char *TAG = LOG_TAG_MESH;
static const uint8_t s_mesh_id[6] = { MESH_ID_0, MESH_ID_1, MESH_ID_2, MESH_ID_3, MESH_ID_4, MESH_ID_5 };

static uint8_t s_rx_buf[KRYOS_RX_BUF_SIZE];
static bool s_mesh_connected = false;
static bool s_mesh_started = false;
static bool s_tasks_started = false;
static bool s_is_leader = false;
static TaskHandle_t s_consensus_task_handle = NULL;
static int s_mesh_layer = -1;
static int16_t s_parent_rssi = -50;
static uint32_t s_consensus_round_id = 0;
static uint32_t s_local_round_id = 0;
static kryos_status_t s_last_status = KRYOS_STATUS_QUORUM_FAIL;
static kryos_node_slot_t s_slots[KRYOS_CONSENSUS_NODE_COUNT];

/* Forward Declarations */
static void consensus_task(void *arg);

/* Helpers */
static uint32_t now_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }
static int32_t c_to_milli(float c) { return (int32_t)(c * 1000.0f); }
static float milli_to_c(int32_t m) { return (float)m / 1000.0f; }
static const char *status_name(kryos_status_t s) {
    switch(s) {
        case KRYOS_STATUS_NOMINAL: return "NOMINAL";
        case KRYOS_STATUS_QUORUM_FAIL: return "QUORUM_FAIL";
        case KRYOS_STATUS_AUTH_FAIL: return "AUTH_FAIL";
        default: return "FAULT";
    }
}

static esp_err_t compute_hmac(const void *data, size_t data_len, uint8_t *hmac_out)
{
    const uint8_t key[] = KRYOS_NODE_PSK;
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id;
    psa_status_t status;
    size_t hmac_len;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_HASH);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attributes, 256);
    status = psa_import_key(&attributes, key, 32, &key_id);
    if (status != PSA_SUCCESS) return ESP_FAIL;
    status = psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256), data, data_len, hmac_out, 32, &hmac_len);
    psa_destroy_key(key_id);
    return (status == PSA_SUCCESS) ? ESP_OK : ESP_FAIL;
}

static esp_err_t verify_hmac(const void *data, size_t data_len, const uint8_t *hmac_in)
{
    const uint8_t key[] = KRYOS_NODE_PSK;
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id;
    psa_status_t status;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attributes, 256);
    status = psa_import_key(&attributes, key, 32, &key_id);
    if (status != PSA_SUCCESS) return ESP_FAIL;
    status = psa_mac_verify(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256), data, data_len, hmac_in, 32);
    psa_destroy_key(key_id);
    return (status == PSA_SUCCESS) ? ESP_OK : ESP_FAIL;
}

static esp_err_t send_mesh_payload(const void *payload, size_t payload_len)
{
    uint8_t msg_type = ((uint8_t *)payload)[3];
    mesh_data_t data = { .data = (uint8_t *)payload, .size = payload_len, .proto = MESH_PROTO_BIN, .tos = MESH_TOS_P2P };
    if (msg_type == KRYOS_MSG_CONSENSUS) {
        if (!s_is_leader) return ESP_OK;
        return esp_mesh_send(NULL, &data, MESH_DATA_TODS, NULL, 0);
    }
    static const mesh_addr_t BCAST = { .addr = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff} };
    return esp_mesh_send(&BCAST, &data, MESH_DATA_P2P, NULL, 0);
}

static void update_slot(uint8_t node_id, int32_t temp_m, uint8_t qual) {
    if (node_id == 0 || node_id > KRYOS_CONSENSUS_NODE_COUNT) return;
    kryos_node_slot_t *s = &s_slots[node_id - 1];
    s->present = true;
    s->temperature_milli_c = temp_m;
    s->quality_score = qual;
    s->last_seen_s = now_s();
    s->sensor_fault = false;
}

static void handle_sensor_reading(const kryos_sensor_frame_t *f) {
    if (verify_hmac(f, offsetof(kryos_sensor_frame_t, hmac), f->hmac) == ESP_OK) {
        update_slot(f->node_id, f->temperature_milli_c, f->quality_score);
    }
}

static void handle_leader_select(const kryos_select_frame_t *f) {
    bool selected = (f->selected_node_id == KRYOS_NODE_ID);
    if (selected && !s_is_leader) {
        ESP_LOGW(TAG, "ROOT SELECTED US. Starting ASQC.");
        s_is_leader = true;
        if (!s_consensus_task_handle) xTaskCreate(consensus_task, "asqc", 4096, NULL, 5, &s_consensus_task_handle);
    } else if (!selected && s_is_leader) {
        ESP_LOGW(TAG, "Abdicated leadership.");
        s_is_leader = false;
        if (s_consensus_task_handle) { vTaskDelete(s_consensus_task_handle); s_consensus_task_handle = NULL; }
    }
}

static void mesh_rx_task(void *arg) {
    mesh_addr_t from;
    int flag;
    while (true) {
        mesh_data_t data = { .data = s_rx_buf, .size = sizeof(s_rx_buf) };
        if (esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0) != ESP_OK) continue;
        uint8_t type = data.data[3];
        if (type == KRYOS_MSG_SENSOR_READING) handle_sensor_reading((kryos_sensor_frame_t*)data.data);
        else if (type == KRYOS_MSG_LEADER_SELECT) handle_leader_select((kryos_select_frame_t*)data.data);
    }
}

static void consensus_task(void *arg) {
    while (true) {
        uint32_t now = now_s();
        int64_t sum = 0; uint8_t count = 0; uint8_t mask = 0;
        update_slot(KRYOS_NODE_ID, c_to_milli(KRYOS_SIM_TEMP_BASE_C + (KRYOS_NODE_ID-1)*KRYOS_SIM_TEMP_STEP_C), 100);
        for (int i=0; i<KRYOS_CONSENSUS_NODE_COUNT; i++) {
            if (s_slots[i].present && (now - s_slots[i].last_seen_s < 15)) {
                sum += s_slots[i].temperature_milli_c; count++; mask |= (1 << i);
            }
        }
        kryos_consensus_frame_t f = { .magic = KRYOS_PROTOCOL_MAGIC, .version = KRYOS_PROTOCOL_VERSION, .type = KRYOS_MSG_CONSENSUS,
            .round_id = ++s_consensus_round_id, .timestamp_s = now, .consensus_milli_c = count ? (int32_t)(sum/count) : 0,
            .node_mask = mask, .verified_count = count, .quorum_ok = (count >= 3), .status = (count >= 3) ? 0 : 2 };
        compute_hmac(&f, offsetof(kryos_consensus_frame_t, hmac), f.hmac);
        send_mesh_payload(&f, sizeof(f));
        ESP_LOGI(TAG, "CONSENSUS round=%" PRIu32 " temp=%.3f C nodes=%d status=%s", f.round_id, milli_to_c(f.consensus_milli_c), count, count>=3?"NOMINAL":"FAIL");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void election_task(void *arg) {
    while (true) {
        if (s_mesh_connected && s_mesh_layer == 2) {
            kryos_election_frame_t f = { .magic = KRYOS_PROTOCOL_MAGIC, .version = KRYOS_PROTOCOL_VERSION, 
                .type = KRYOS_MSG_LEADER_ELECT, .node_id = KRYOS_NODE_ID, .rssi_dbm = -50 };
            mesh_data_t d = { .data = (uint8_t *)&f, .size = sizeof(f), .proto = MESH_PROTO_BIN, .tos = MESH_TOS_P2P };
            esp_mesh_send(NULL, &d, MESH_DATA_TODS, NULL, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void sensor_task(void *arg) {
    while (true) {
        if (s_mesh_connected) {
            kryos_sensor_frame_t f = { .magic = KRYOS_PROTOCOL_MAGIC, .version = KRYOS_PROTOCOL_VERSION, .type = KRYOS_MSG_SENSOR_READING,
                .node_id = KRYOS_NODE_ID, .quality_score = 90, .round_id = ++s_local_round_id, .timestamp_s = now_s(),
                .temperature_milli_c = c_to_milli(KRYOS_SIM_TEMP_BASE_C + (KRYOS_NODE_ID-1)*KRYOS_SIM_TEMP_STEP_C) };
            compute_hmac(&f, offsetof(kryos_sensor_frame_t, hmac), f.hmac);
            send_mesh_payload(&f, sizeof(f));
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void mesh_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (id == MESH_EVENT_PARENT_CONNECTED) { s_mesh_connected = true; s_mesh_layer = ((mesh_event_connected_t*)data)->self_layer; }
    else if (id == MESH_EVENT_PARENT_DISCONNECTED) { s_mesh_connected = false; }
}

void app_main(void) {
    nvs_flash_init(); psa_crypto_init();
    esp_netif_init(); esp_event_loop_create_default();
    esp_netif_t *sta = NULL; esp_netif_create_default_wifi_mesh_netifs(&sta, NULL);
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT(); esp_wifi_init(&wcfg);
    esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID, &mesh_event_handler, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_FLASH); esp_wifi_start();
    esp_mesh_init(); esp_mesh_set_topology(MESH_TOPO_TREE);
    mesh_cfg_t mcfg = MESH_INIT_CONFIG_DEFAULT(); 
    mcfg.router.ssid_len = 5; memcpy(mcfg.router.ssid, "KRYOS", 5);
    memcpy(mcfg.mesh_id.addr, s_mesh_id, 6); mcfg.channel = 6;
    esp_mesh_set_config(&mcfg); esp_mesh_set_self_organized(true, true); esp_mesh_start();
    s_mesh_started = true;
    xTaskCreate(mesh_rx_task, "rx", 4096, NULL, 6, NULL);
    xTaskCreate(sensor_task, "sensor", 4096, NULL, 5, NULL);
    xTaskCreate(election_task, "elect", 4096, NULL, 5, NULL);
}
