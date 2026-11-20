/** @file ftel_isp_auto.h
 * @brief OS-independent, single-owner interface to the binary 3A package.
 * @details Configure before isp_init. Collect only after frame completion;
 * apply WB after release and before the next submit, with hardware idle.
 * Sensor exposure/gain are proposals only: this interface does not claim an
 * I2C write or a sensor control has taken effect. The caller supplies the
 * controls actually effective on every measured frame.
 */
#ifndef __NEON_FTEL_ISP_AUTO_H__
#define __NEON_FTEL_ISP_AUTO_H__
#include "ftel_isp.h"
#include "pipeline/ftel_isp_init.h"
#ifdef __cplusplus
extern "C" {
#endif

#define FTEL_ISP_AUTO_REPLAY 1u
#define FTEL_ISP_AUTO_REPLAY_UNSCREENED_AWB 2u
#define FTEL_ISP_AUTO_RESULT_REPLAY 1u
#define FTEL_ISP_AUTO_RESULT_AWB_HELD 2u
#define FTEL_ISP_AUTO_RESULT_UNSCREENED_AWB 4u
#define FTEL_ISP_AUTO_RESULT_LUX_VALID 8u
#define FTEL_ISP_AUTO_RESULT_CCT_VALID 16u
#define FTEL_ISP_AUTO_RESULT_CCT_INITIAL 32u
typedef struct {
    uint32_t exposure_lines, analog_gain_q8, wb_red_q8, wb_blue_q8;
} ftel_isp_auto_controls_t;
typedef struct {
    uint32_t frame_id, context_id, flags, current_luma;
    int32_t lux_index_q16_16;
    uint32_t cct_kelvin, awb_valid_rois;
    ftel_isp_auto_controls_t proposed;
} ftel_isp_auto_result_t;
typedef struct {
    void *memory;
    size_t memory_bytes;
} ftel_isp_auto_t;
size_t ftel_isp_auto_memory_size(void);
size_t ftel_isp_auto_memory_alignment(void);
/* Pure RAM setup: preserves the pipeline profile except stats and initial WB.
 * Binaries must remain alive. Config must describe the actual replay input.
 * Strict mode rejects camera-mode mismatch; replay allows reference proposals.
 * On success initial sensor controls are reference metadata until confirmed by
 * a real sensor integration. No sensor register sequence is executed here.
 */
ftel_isp_status_t ftel_isp_auto_init(ftel_isp_auto_t *handle, void *memory, size_t bytes,
    const void *sensor_bin, size_t sensor_bytes, const void *tuning_bin, size_t tuning_bytes,
    uint32_t flags, isp_config_t *config, ftel_isp_auto_controls_t *initial);
/* Nonblocking. Missing AEC/AWB bank returns NO_DATA; retry without resubmitting
 * the frame. Copies are retained across partial AEC/AWB readiness. Timeout is
 * owned by the caller. A failed read never consumes another owner's lease.
 */
ftel_isp_status_t ftel_isp_auto_process(ftel_isp_auto_t *handle,
    const ftel_isp_auto_controls_t *effective, ftel_isp_auto_result_t *result);
/* Applies only WB. Caller must release outputs, stop queuing input, and own ISP.
 * A partial hardware failure invalidates this auto context; drain/recover ISP.
 */
ftel_isp_status_t ftel_isp_auto_apply_wb(ftel_isp_auto_t *handle,
    const ftel_isp_auto_result_t *result, ftel_isp_auto_controls_t *effective);
void ftel_isp_auto_reset(ftel_isp_auto_t *handle);
#ifdef __cplusplus
}
#endif
#endif
