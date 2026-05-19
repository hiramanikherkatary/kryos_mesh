#ifndef ASQC_PROTOCOL_H
#define ASQC_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

/**
 * @file asqc_protocol.h
 * @brief ASQC (Adaptive Signal Quality Control) Protocol Implementation
 * 
 * This protocol ensures reliable mesh communication by monitoring signal quality
 * and adapting transmission strategies based on link quality assessment.
 */

/* ==== ASQC Data Structure ==== */

/**
 * @struct asqc_data_t
 * @brief ASQC protocol data packet
 * 
 * Contains temperature reading with signal quality assessment
 */
typedef struct {
    int rssi;                    /**< Signal strength in dBm (-120 to -30) */
    uint8_t node_id;             /**< Source node ID (1-254) */
    float temperature;           /**< Temperature value in °C */
    uint32_t timestamp;          /**< Unix timestamp of measurement */
    uint8_t retry_count;         /**< Number of transmission retries */
    uint8_t quality_score;       /**< Signal quality 0-100% */
} asqc_data_t;

/* ==== ASQC Quality Assessment ==== */

/**
 * @brief Quality score levels
 */
typedef enum {
    QUALITY_LEVEL_POOR      = 0,    // 0-49%   - Poor quality
    QUALITY_LEVEL_FAIR      = 1,    // 50-69%  - Fair quality
    QUALITY_LEVEL_GOOD      = 2,    // 70-89%  - Good quality
    QUALITY_LEVEL_EXCELLENT = 3,    // 90-100% - Excellent quality
} asqc_quality_level_t;

/**
 * @brief ASQC status codes
 */
typedef enum {
    ASQC_OK                 = 0,    // Operation successful
    ASQC_LOW_SIGNAL         = 1,    // Signal below threshold
    ASQC_NO_CONNECTION      = 2,    // Not connected to mesh
    ASQC_RETRY_FAILED       = 3,    // Retries exhausted
    ASQC_INVALID_TEMP       = 4,    // Invalid temperature value
    ASQC_ERROR              = 5,    // General error
} asqc_status_t;

/* ==== Function Declarations ==== */

/**
 * @brief Calculate quality score from RSSI value
 * 
 * @param rssi Signal strength in dBm
 * @return Quality score (0-100%)
 */
uint8_t asqc_calculate_quality_score(int rssi);

/**
 * @brief Get quality level from score
 * 
 * @param quality_score Quality score (0-100)
 * @return Quality level enum
 */
asqc_quality_level_t asqc_get_quality_level(uint8_t quality_score);

/**
 * @brief Get quality level as string
 * 
 * @param level Quality level
 * @return String representation ("Excellent", "Good", "Fair", "Poor")
 */
const char* asqc_quality_level_to_string(asqc_quality_level_t level);

/**
 * @brief Check if signal quality is acceptable
 * 
 * @param rssi Signal strength in dBm
 * @param threshold Minimum acceptable RSSI
 * @return true if acceptable, false otherwise
 */
bool asqc_is_signal_acceptable(int rssi, int threshold);

/**
 * @brief Map RSSI to quality percentage with logarithmic scaling
 * 
 * Maps RSSI (-120 to -30 dBm) to 0-100% quality
 * - -30 dBm = 100% (excellent)
 * - -75 dBm = 50% (fair)
 * - -120 dBm = 0% (poor)
 * 
 * @param rssi Signal strength in dBm
 * @return Quality percentage (0-100)
 */
uint8_t asqc_rssi_to_quality_percent(int rssi);

/**
 * @brief Initialize ASQC packet with default values
 * 
 * @param packet Pointer to asqc_data_t packet
 * @param node_id Node identifier
 * @return ASQC_OK on success
 */
asqc_status_t asqc_init_packet(asqc_data_t *packet, uint8_t node_id);

/**
 * @brief Fill ASQC packet with current values
 * 
 * @param packet Pointer to asqc_data_t packet
 * @param temperature Temperature reading
 * @param rssi Current RSSI value
 * @return ASQC_OK on success
 */
asqc_status_t asqc_fill_packet(asqc_data_t *packet, float temperature, int rssi);

/**
 * @brief Validate temperature value
 * 
 * @param temp Temperature value to validate
 * @return ASQC_OK if valid, ASQC_INVALID_TEMP otherwise
 */
asqc_status_t asqc_validate_temperature(float temp);

/**
 * @brief Get recommended action based on RSSI
 * 
 * Returns action to take:
 * - ASQC_OK: No action needed, transmit normally
 * - ASQC_LOW_SIGNAL: Reduce transmit rate or retry
 * - ASQC_NO_CONNECTION: Cannot transmit, wait for connection
 * - ASQC_RETRY_FAILED: Max retries exceeded, log error
 * 
 * @param rssi Current RSSI value
 * @return Recommended action status
 */
asqc_status_t asqc_get_transmission_action(int rssi);

/**
 * @brief Print ASQC packet to log
 * 
 * @param packet Pointer to asqc_data_t packet
 * @param tag Log tag to use
 */
void asqc_log_packet(const asqc_data_t *packet, const char *tag);

/**
 * @brief Get RSSI description string
 * 
 * @param rssi Signal strength in dBm
 * @return Description string
 */
const char* asqc_get_rssi_description(int rssi);

/* ==== ASQC Statistics ==== */

/**
 * @struct asqc_stats_t
 * @brief ASQC statistics for monitoring
 */
typedef struct {
    uint32_t packets_sent;       // Total packets sent
    uint32_t packets_failed;     // Failed packets
    uint32_t retries_performed;  // Total retries
    int average_rssi;            // Average RSSI over time
    uint8_t average_quality;     // Average quality score
} asqc_stats_t;

/**
 * @brief Update ASQC statistics
 * 
 * @param stats Pointer to asqc_stats_t structure
 * @param packet Pointer to asqc_data_t packet
 * @param success true if transmission successful
 */
void asqc_update_stats(asqc_stats_t *stats, const asqc_data_t *packet, bool success);

/**
 * @brief Reset ASQC statistics
 * 
 * @param stats Pointer to asqc_stats_t structure
 */
void asqc_reset_stats(asqc_stats_t *stats);

/**
 * @brief Print ASQC statistics
 * 
 * @param stats Pointer to asqc_stats_t structure
 * @param tag Log tag to use
 */
void asqc_log_stats(const asqc_stats_t *stats, const char *tag);

#endif // ASQC_PROTOCOL_H
