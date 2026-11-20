/** @file fpt_3a.h
 * @brief Standalone binary-distribution ABI. No ISP SDK or hardware headers.
 * All calls are serialized by one owner. Binary storage and aligned context
 * memory belong to the caller and must survive until reset/reinitialization.
 */
#ifndef FPT_3A_H
#define FPT_3A_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define FPT_3A_ABI_VERSION 1u
#define FPT_3A_MAX_ZONES 768u
#define FPT_3A_HIST_BINS 64u
/* Bayer encoding is shared by FTEL and the camera package: RGGB=0, GRBG=1,
 * GBRG=2, BGGR=3. raw_bits describes capture, not a uint16 storage width. */
#define FPT_3A_REPLAY 1u
/* Explicit diagnostic override, never for a production camera. */
#define FPT_3A_REPLAY_UNSCREENED_AWB 2u
#define FPT_3A_RESULT_REPLAY 1u
#define FPT_3A_RESULT_AWB_HELD 2u
#define FPT_3A_RESULT_UNSCREENED_AWB 4u
#define FPT_3A_RESULT_LUX_VALID 8u
#define FPT_3A_RESULT_CCT_VALID 16u
#define FPT_3A_RESULT_CCT_INITIAL 32u

typedef enum {
    FPT_3A_OK = 0, FPT_3A_E_PARAM = -1, FPT_3A_E_BINARY = -2,
    FPT_3A_E_MODE = -3, FPT_3A_E_STATS = -4, FPT_3A_E_STATE = -5
} fpt_3a_status_t;
typedef struct {
    uint32_t abi_version, width, height, raw_bits, bayer_pattern, flags;
} fpt_3a_config_t;
typedef struct {
    uint32_t exposure_lines, analog_gain_q8, wb_red_q8, wb_blue_q8;
} fpt_3a_controls_t;
typedef struct {
    uint32_t sensor_width, sensor_height, sensor_raw_bits, sensor_bayer_pattern;
    uint32_t aec_columns, aec_rows, awb_columns, awb_rows;
    uint32_t awb_ue_raw12, awb_oe_raw12, flags;
    fpt_3a_controls_t initial;
} fpt_3a_info_t;
/* Raw sums are always in the FTEL normalized RAW12 hardware domain. */
typedef struct {
    uint64_t sum;
    uint32_t count, sample_count, ue_count, oe_count;
} fpt_3a_aec_zone_t;
typedef struct {
    uint64_t red_sum, green_sum, blue_sum;
    uint32_t count, sample_count, ue_count, oe_count;
} fpt_3a_awb_zone_t;
typedef struct {
    uint32_t frame_id, context_id;
    uint64_t aec_sum;
    uint32_t aec_count, sample_count, ue_count, oe_count;
    uint32_t aec_zone_count, awb_zone_count;
    const fpt_3a_aec_zone_t *aec_zones;
    const fpt_3a_awb_zone_t *awb_zones;
    const uint32_t *histogram;
    /* True only with real scalar Bayer populations for EVERY AWB ROI. */
    uint32_t awb_populations_valid;
} fpt_3a_stats_t;
typedef struct {
    uint32_t frame_id, context_id, flags, current_luma;
    int32_t lux_index_q16_16;
    uint32_t cct_kelvin, awb_valid_rois;
    fpt_3a_controls_t proposed;
} fpt_3a_result_t;

size_t fpt_3a_context_size(void);
size_t fpt_3a_context_alignment(void);
fpt_3a_status_t fpt_3a_init(void *memory, size_t bytes,
    const fpt_3a_config_t *config, const void *sensor_bin, size_t sensor_bytes,
    const void *tuning_bin, size_t tuning_bytes, fpt_3a_info_t *info);
/* effective describes the measured frame, never the last unconfirmed request.
 * Replay produces reference AE proposals only: RAW exposure is immutable.
 * On error temporal history is unchanged. On success it advances once.
 */
fpt_3a_status_t fpt_3a_process(void *memory, const fpt_3a_stats_t *stats,
    const fpt_3a_controls_t *effective, fpt_3a_result_t *result);
/* Invalidates the context; call init again with live binaries before reuse. */
void fpt_3a_reset(void *memory);
#ifdef __cplusplus
}
#endif
#endif
