/**
 * @file neon_isp_hw.h
 * @brief @internal Register-level contract of the ISP backends.
 * @details
 * neon_isp.c holds every rule of the module, including validation, buffer rotation and ownership
 * bookkeeping, and is the same for every chip. A chip supplies these functions in exactly one backend: the
 * in-RAM simulation (fx1_isp_simulation.c) or its register backend (the FX1 one does not exist yet), plus
 * its limits through neon_isp_hw_get_caps(). Applications must not include this file.
 *
 * A backend never validates what the core has validated, never keeps a copy of the books, and never
 * blocks: it writes and reads registers and returns.
 */

#ifndef __NEON_ISP_HW_H__
#define __NEON_ISP_HW_H__

/*==================================================================================================
*                                          INCLUDE FILES                                          
==================================================================================================*/
#include "isp/neon_isp.h"
#ifdef __cplusplus
extern "C" {
#endif

/*==================================================================================================
*                                       FUNCTION PROTOTYPES                                       
==================================================================================================*/
/** Limits of the chip; FX1 defines it in fx1_isp_caps.c, shared by its backends. */
neon_status_t neon_isp_hw_get_caps(neon_isp_caps_t *caps);
/** The handle holds the caps already. */
neon_status_t neon_isp_hw_init(neon_isp_handle_t *handle);
/**
 * Program geometry, Bayer pattern, strides, the output buffer bases, the burst length, the statistics
 * generators and the event mask, with the pipeline and both DMA engines disabled. Leaves every buffer
 * ownership bit clear, so that nothing is queued and nothing is free.
 */
neon_status_t neon_isp_hw_configure(neon_isp_handle_t *handle, const neon_isp_config_t *config);
/**
 * Enable the pipeline first, then mark every output buffer free, then enable both DMA engines (MAS 9.1.1:
 * the output geometry is only driven while the pipeline is enabled). Inputs already queued stay valid.
 */
neon_status_t neon_isp_hw_start(neon_isp_handle_t *handle);
/** Disable both DMA engines, wait for them to drain, disable the pipeline, clear every ownership bit. */
neon_status_t neon_isp_hw_stop(neon_isp_handle_t *handle);
/**
 * Pulse the soft reset (COMMON_CTRL.SOFT_RST) and wait for it to finish. The configuration survives; the
 * frame in flight, the ownership bits and the statistics results do not. Afterwards the backend marks every
 * output buffer free again and leaves the pipeline enabled, with no input queued.
 */
neon_status_t neon_isp_hw_soft_reset(neon_isp_handle_t *handle);
neon_status_t neon_isp_hw_read_events(neon_isp_handle_t *handle, uint32_t *events);
/** Write one to the given status bits. */
neon_status_t neon_isp_hw_clear_events(neon_isp_handle_t *handle, uint32_t events);
neon_status_t neon_isp_hw_set_event_mask(neon_isp_handle_t *handle, uint32_t event_mask);
/** Write the slot's base address, tag and timestamp, then set its valid bit. */
neon_status_t neon_isp_hw_queue_input(neon_isp_handle_t *handle, uint8_t slot, const neon_isp_input_t *input);
/** IDMA_BUF_VALID and ODMA_BUF_DONE, one bit per slot. */
neon_status_t neon_isp_hw_read_buffers(neon_isp_handle_t *handle, uint8_t *input_valid_mask, uint8_t *output_done_mask);
/** Tag, timestamp and the two clock counts published with a completed output slot. The core fills the rest. */
neon_status_t neon_isp_hw_read_output(neon_isp_handle_t *handle, uint8_t slot, neon_isp_output_t *output);
/** Clear the slot's done bit, and only then set its free bit. */
neon_status_t neon_isp_hw_release_output(neon_isp_handle_t *handle, uint8_t slot);
/**
 * The raw results of every generator: aec, awb and af, each with valid, frame_id and context_id. Acquire
 * and release the AWB bank inside this call. Leave the top-level frame_id, context_id, means and focus
 * metric to the core.
 */
neon_status_t neon_isp_hw_read_statistics(neon_isp_handle_t *handle, neon_isp_statistics_t *statistics);
/** Write AEC_CONTEXT_ID, AWB_CONTEXT_ID and AF_CONTEXT_ID. */
neon_status_t neon_isp_hw_set_context_id(neon_isp_handle_t *handle, uint32_t context_id);
/** Write the white balance gains; the pipeline samples them at the next start of frame. */
neon_status_t neon_isp_hw_apply_controls(neon_isp_handle_t *handle, const neon_isp_dynamic_controls_t *controls);
/** frames_in and frames_out of the status; leaves the rest alone. */
neon_status_t neon_isp_hw_read_counters(neon_isp_handle_t *handle, neon_isp_status_t *status);

/* What a tuning profile may do to one register offset. A fact of the chip's register map, the same for every
 * backend of that chip, so it needs no handle. */
typedef enum {
    NEON_ISP_HW_PROFILE_WRITE = 0,     /* A tuning register: write it. */
    NEON_ISP_HW_PROFILE_SKIP = 1,      /* Programmed by the HAL itself: leave it out. */
    NEON_ISP_HW_PROFILE_REJECT = 2     /* Not a register, read-only, or clears status: the profile is wrong. */
} neon_isp_hw_profile_action_t;

neon_isp_hw_profile_action_t neon_isp_hw_profile_classify(uint32_t offset);
/* One register write of a profile, in file order. Called only while the pipeline is stopped. */
neon_status_t neon_isp_hw_profile_write(neon_isp_handle_t *handle, uint32_t offset, uint32_t value);

#ifdef __cplusplus
}
#endif

#endif /* __NEON_ISP_HW_H__ */
