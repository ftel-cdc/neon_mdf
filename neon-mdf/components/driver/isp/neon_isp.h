/**
 * @file neon_isp.h
 * @brief Public declarations for the ISP.
 * @details
 * RAW to NV12 image signal processor in the memory-to-memory topology: unpacked Bayer frames are read
 * from DDR by the input DMA, processed, and written to DDR as NV12 by the output DMA. A frame is read once
 * and written once; the ISP has no sensor, CSI-2 or lens interface. The CSI-2 receiver (neon_csi.h) writes
 * the frames the ISP reads, but the two modules share nothing except DDR: the caller hands buffers from
 * one to the other (see neon_camera_runtime).
 *
 * This header holds the contract only. Every number that belongs to a chip (buffer counts, frame size
 * limits, alignment, burst limit, which statistics exist, which events exist) is reported by
 * neon_isp_get_caps() and enforced from there, so that another chip changes its backend and not this file.
 * The first chip is the FX1 FTEL ISP (MAS v1.0, register map 1.07).
 *
 * A tuning profile (lens shading mesh, gamma, tone mapping, edge enhancement, ...) is a register image
 * written by neon_isp_load_profile() while the ISP is stopped. Without one, blocks that need a table stay in
 * their reset state. The 3A loop only needs the global statistics below; per-zone results and the Resizer are
 * not exposed.
 * @note Blocking is never done inside a call: every function returns at once. Calls are intended for task
 * context. neon_isp_get_events() and neon_isp_clear_events() are the only calls meant for an interrupt
 * handler.
 */

#ifndef __NEON_ISP_H__
#define __NEON_ISP_H__

/*==================================================================================================
*                                          INCLUDE FILES                                          
==================================================================================================*/
#include <stdbool.h>
#include <stdint.h>
#include "neon_types.h"
#include "isp/neon_isp_events.h"
#ifdef __cplusplus
extern "C" {
#endif

/*==================================================================================================
*                                        DEFINES AND MACROS                                       
==================================================================================================*/
/*
 * Capacity of the structures below: the most that any supported chip needs. What a chip really has is in
 * neon_isp_caps_t and is never larger. A chip that needs more raises these.
 */
#define NEON_ISP_MAX_BUFFERS 8U       /* DMA buffers per engine (MAS: 2 to 8). */
#define NEON_ISP_MAX_HIST_BINS 64U    /* Auto-exposure histogram bins. */
#define NEON_ISP_BACKEND_WORDS 96U    /* Words of backend-private storage inside the handle. */

/*==================================================================================================
*                                  STRUCTURES AND OTHER TYPEDEFS                                  
==================================================================================================*/
typedef enum {
    NEON_ISP_RAW10 = 10,
    NEON_ISP_RAW12 = 12
} neon_isp_raw_depth_t;

/** Colour of the first four samples of the frame, in raster order. */
typedef enum {
    NEON_ISP_BAYER_RGGB = 0,
    NEON_ISP_BAYER_GRBG = 1,
    NEON_ISP_BAYER_GBRG = 2,
    NEON_ISP_BAYER_BGGR = 3
} neon_isp_bayer_pattern_t;

typedef enum {
    NEON_ISP_STATE_IDLE = 0,        /* Initialized, not configured. */
    NEON_ISP_STATE_CONFIGURED = 1,
    NEON_ISP_STATE_RUNNING = 2
} neon_isp_state_t;

typedef enum {
    /*
     * Reset the pixel pipeline and both DMA engines. The configuration is kept; the frame in flight, the
     * buffer ownership and the statistics results are discarded (MAS 9.1.1). Needs no full reset.
     */
    NEON_ISP_RECOVER_SOFT = 0
} neon_isp_recover_level_t;

/** What one chip provides. The core validates and schedules from these values only. */
typedef struct {
    uint8_t input_buffers;        /* Input DMA rotation, at most NEON_ISP_MAX_BUFFERS. */
    uint8_t output_buffers;       /* Output DMA rotation, at most NEON_ISP_MAX_BUFFERS. */
    uint8_t aec_hist_bins;        /* At most NEON_ISP_MAX_HIST_BINS; 0 when there is no auto-exposure block. */
    bool aec;                     /* Statistics generators that exist. */
    bool awb;
    bool af;
    uint16_t raw_depth_mask;      /* Bit n set: RAW<n> is accepted. */
    uint8_t bayer_pattern_mask;   /* Bit n set: neon_isp_bayer_pattern_t value n is accepted. */
    uint16_t min_width;           /* Frame size the input DMA reads, in samples and lines. */
    uint16_t max_width;
    uint16_t min_height;
    uint16_t max_height;
    uint8_t size_granularity;     /* Width and height are multiples of this. */
    uint16_t max_burst_beats;     /* DMA burst length, 1 to this. */
    /*
     * Base address and line stride of every buffer are multiples of this: the width in bytes of the
     * AXI data port, which is an integration parameter of the chip and not a register.
     */
    uint32_t dma_align_bytes;
    uint16_t max_wb_gain_q8;      /* White balance gain, UQ4.8, 0x100 = 1.0. */
    uint32_t supported_events;    /* NEON_ISP_EVENT_* bits that the chip can raise. */
} neon_isp_caps_t;

/*
 * Buffers. Input samples are 16-bit little-endian halfwords, one per Bayer sample, MSB-aligned in 12 bits:
 * RAW12 occupies bits 11:0 and RAW10 is {raw[9:0], 2'b00}; bits 15:12 are zero (MAS 6.6.3.2, 6.25.3.2).
 * Output is NV12: a luma plane of one byte per pixel and an interleaved UV plane with one pair per
 * 2x2 block, height / 2 lines of width bytes each.
 */
typedef struct {
    uint16_t width;                    /* Samples per line that the input DMA reads. */
    uint16_t height;
    neon_isp_raw_depth_t raw_depth;
    neon_isp_bayer_pattern_t bayer_pattern;
    uint32_t input_stride_bytes;       /* At least 2 * width, a multiple of caps.dma_align_bytes. */
    uint32_t y_stride_bytes;           /* At least width, a multiple of caps.dma_align_bytes. */
    uint32_t uv_stride_bytes;          /* At least width, a multiple of caps.dma_align_bytes. */
    /* One luma and one chroma buffer per output rotation slot, caps.output_buffers of each. */
    uint32_t output_y_addr[NEON_ISP_MAX_BUFFERS];
    uint32_t output_uv_addr[NEON_ISP_MAX_BUFFERS];
    uint32_t output_y_bytes;           /* Size of each luma buffer, at least y_stride_bytes * height. */
    uint32_t output_uv_bytes;          /* Size of each chroma buffer, at least uv_stride_bytes * height / 2. */
    uint16_t max_burst_beats;          /* 1 to caps.max_burst_beats. */
    bool stats_aec;                    /* Run the statistics generators; a generator the chip lacks is refused. */
    bool stats_awb;
    bool stats_af;
    uint32_t event_mask;               /* Events that may assert the interrupt line. */
} neon_isp_config_t;

/** One frame handed to the input DMA. */
typedef struct {
    uint32_t address;                  /* Base of the frame, a multiple of caps.dma_align_bytes. */
    uint32_t size_bytes;               /* At least input_stride_bytes * height. */
    uint32_t tag;                      /* Software tag, carried to the output frame untouched. */
    uint32_t timestamp;                /* Capture time in the caller's time base, carried untouched. */
    /* Set by neon_isp_reclaim_input(): false when the frame was thrown away by stop or recover. */
    bool processed;
} neon_isp_input_t;

/**
 * What a tuning profile holds, as counted by neon_isp_check_profile() and neon_isp_load_profile().
 *
 * A profile is the register image of one tuning set (MAS 9.5.3): every block's configuration for one sensor
 * mode and one light source. Its binary form is a list of 8-byte entries, each a register offset from the ISP
 * base and the value to write, both 32-bit little endian, with no header. Entries are written in file order,
 * which matters: a table is loaded by a command followed by a run of writes to one data register.
 */
typedef struct {
    uint32_t entries;                  /* Register writes in the profile. */
    uint32_t written;                  /* Writes that go to the hardware. */
    uint32_t skipped;                  /* Writes to registers the HAL owns (geometry, DMA, interrupts, ...). */
} neon_isp_profile_info_t;

#define NEON_ISP_PROFILE_ENTRY_BYTES 8U

/** One completed NV12 frame. */
typedef struct {
    uint8_t buffer_index;
    uint32_t y_addr;
    uint32_t uv_addr;
    uint32_t y_stride_bytes;
    uint32_t uv_stride_bytes;
    uint16_t width;
    uint16_t height;
    uint32_t tag;                      /* Of the input frame this one was made from. */
    uint32_t timestamp;                /* Of the input frame this one was made from. */
    uint32_t sof_count;                /* Core clock count at the start of the output frame, modulo 2^32. */
    uint32_t eof_count;                /* Same at the end; eof_count - sof_count is the processing span. */
} neon_isp_output_t;

/** Auto exposure, measured after bad pixel correction and before white balance. */
typedef struct {
    bool valid;                        /* A result exists for this generator. */
    uint32_t frame_id;                 /* Frames that entered the pipeline since reset; the first is 1. */
    uint32_t context_id;               /* The tag set by neon_isp_set_context_id() before that frame began. */
    uint64_t sum;                      /* Sum of the samples counted. */
    uint64_t count;
    uint64_t under_count;              /* Samples below the under-exposure threshold. */
    uint64_t over_count;               /* Samples above the over-exposure threshold. */
    bool overflow;
    bool histogram_valid;              /* False when a new frame began while the histogram was read. */
    uint32_t histogram[NEON_ISP_MAX_HIST_BINS]; /* caps.aec_hist_bins bins are used. */
} neon_isp_aec_stats_t;

/** Auto white balance, measured after demosaic. */
typedef struct {
    bool valid;
    uint32_t frame_id;
    uint32_t context_id;
    uint64_t sum_r;
    uint64_t sum_g;
    uint64_t sum_b;
    uint32_t count;                    /* Cells that took part; the sums are over these. */
    bool overrun;                      /* A result was lost because no bank was free. */
} neon_isp_awb_stats_t;

/** Auto focus, measured on luma after colour space conversion. */
typedef struct {
    bool valid;
    uint32_t frame_id;
    uint32_t context_id;
    uint64_t focus;                    /* Combined focus value over the grid. */
} neon_isp_af_stats_t;

/**
 * Global statistics of the newest frame of each generator. The three generators publish at their own end
 * of frame, so their frame_id may differ by one when a read falls between them.
 */
typedef struct {
    /*
     * Of the first valid generator: auto exposure, white balance, focus. The generators publish separately:
     * NEON_OK means one of them is newer, so this id (the exposure result's) may be one returned before.
     */
    uint32_t frame_id;
    uint32_t context_id;
    /*
     * Means for a 3A that wants no more than that, derived by the core from the sums on a 0 to 255 scale:
     * (sum / count) >> 4 on the 12-bit Bayer range. Zero when the generator has no valid result.
     */
    uint8_t mean_luma;                 /* From the auto-exposure sum. */
    uint8_t mean_red;                  /* From the auto-white-balance sums. */
    uint8_t mean_green;
    uint8_t mean_blue;
    uint16_t focus_metric;             /* The focus value, saturated at 65535. */
    neon_isp_aec_stats_t aec;
    neon_isp_awb_stats_t awb;
    neon_isp_af_stats_t af;
} neon_isp_statistics_t;

/** Controls that take effect at the next start of frame, without stopping the pipeline. */
typedef struct {
    uint32_t target_frame_id;          /* Frame the caller wants it on. The ISP cannot delay: see apply. */
    uint16_t wb_red_gain_q8;           /* UQ4.8, 0x100 = 1.0; 1 to caps.max_wb_gain_q8. */
    uint16_t wb_blue_gain_q8;
} neon_isp_dynamic_controls_t;

typedef struct {
    neon_isp_state_t state;
    uint8_t input_queued_mask;         /* Input slots queued and not yet consumed by the DMA. */
    uint8_t output_done_mask;          /* Output slots that hold a frame software has not taken. */
    uint8_t output_held_mask;          /* Output slots held by software. */
    uint32_t frames_in;                /* Frames that entered the pipeline. */
    uint32_t frames_out;               /* Frames that completed in memory. */
    bool irq_pending;                  /* A set event is enabled in event_mask. */
} neon_isp_status_t;

typedef struct neon_isp_handle_s {
    neon_isp_caps_t caps;              /* Copied from the backend by neon_isp_init(). */
    neon_isp_state_t state;
    uint16_t width;
    uint16_t height;
    uint32_t input_stride_bytes;
    uint32_t y_stride_bytes;
    uint32_t uv_stride_bytes;
    uint32_t output_y_addr[NEON_ISP_MAX_BUFFERS];
    uint32_t output_uv_addr[NEON_ISP_MAX_BUFFERS];
    neon_isp_input_t input[NEON_ISP_MAX_BUFFERS]; /* Books of the queued inputs, by rotation slot. */
    uint8_t input_head;                /* Rotation slot that the next queued input uses. */
    uint8_t input_oldest;              /* Oldest slot not yet reclaimed. */
    uint8_t input_count;               /* Slots queued and not reclaimed. */
    uint8_t input_discard_mask;        /* Slots that stop or recover threw away. */
    bool input_flushing;               /* Stop or recover left inputs to reclaim; queue only after they are back. */
    uint8_t output_held_mask;          /* Slots taken by neon_isp_acquire_output(). */
    uint8_t output_next;               /* Slot searched first by the next acquire. */
    uint32_t event_mask;
    uint32_t stats_frame_id[3];        /* Newest frame id handed out for AEC, AWB, AF. */
    uint64_t backend_words[NEON_ISP_BACKEND_WORDS]; /* Private to the selected backend. */
} neon_isp_handle_t;

/*==================================================================================================
*                                       FUNCTION PROTOTYPES                                       
==================================================================================================*/
/**
 * @brief Prepare a handle and reset the backend state. Does not touch the pipeline.
 * @param[out] handle ISP handle, caller allocated.
 * @return NEON_OK on success, NEON_INVALID_PARAM for a NULL pointer, NEON_ERROR if the backend caps are
 * unusable, or a backend status.
 */
neon_status_t neon_isp_init(neon_isp_handle_t *handle);

/**
 * @brief Report what the chip provides. Needs no handle and no hardware.
 * @param[out] caps Capabilities.
 * @return NEON_OK, or NEON_INVALID_PARAM for a NULL pointer.
 */
neon_status_t neon_isp_get_caps(neon_isp_caps_t *caps);

/**
 * @brief Release the handle.
 * @param[in] handle ISP handle.
 * @return NEON_OK, or NEON_BUSY while the pipeline is running.
 */
neon_status_t neon_isp_deinit(neon_isp_handle_t *handle);

/**
 * @brief Fill a configuration with portable defaults: RAW12, RGGB, 16-beat bursts, every statistics
 * generator on and the frame, DMA and error events enabled. The geometry, strides and buffers are left at
 * zero for the caller, and a generator that the chip lacks must be switched off.
 * @param[out] config Configuration.
 */
void neon_isp_get_default_config(neon_isp_config_t *config);

/**
 * @brief Validate and program a configuration. The pipeline stays disabled. Frames that were queued and
 * never reclaimed are forgotten.
 * @param[in,out] handle ISP handle, not running.
 * @param[in] config Configuration.
 * @return NEON_OK on success, NEON_BUSY while running, NEON_INVALID_PARAM for an invalid configuration,
 * NEON_NOT_SUPPORTED for a depth, pattern or statistics generator that the chip lacks, or a backend status.
 */
neon_status_t neon_isp_configure(neon_isp_handle_t *handle, const neon_isp_config_t *config);

/**
 * @brief Enable the pipeline and then both DMA engines, with every output buffer free (MAS 9.1.1 steps 7
 * and 8: the output geometry is driven only while the pipeline is enabled). Inputs queued before the
 * call are processed first. Start the sensor after it.
 * @param[in,out] handle ISP handle, configured.
 * @return NEON_OK on success, NEON_BUSY if running, NEON_ERROR if not configured, or a backend status.
 */
neon_status_t neon_isp_start(neon_isp_handle_t *handle);

/**
 * @brief Disable both DMA engines, drain, and disable the pipeline. Stop the sensor first. Output frames
 * held by software or not yet taken become invalid; inputs still queued are reported by
 * neon_isp_reclaim_input() as not processed.
 * @param[in,out] handle ISP handle, running.
 * @return NEON_OK on success, NEON_ERROR if the pipeline is not running.
 */
neon_status_t neon_isp_stop(neon_isp_handle_t *handle);

/**
 * @brief Reset the pipeline and keep the configuration, then enable it again. Output frames held by
 * software are lost, every output buffer is free again, and inputs still queued are reported as not
 * processed.
 * @param[in,out] handle ISP handle, running.
 * @param[in] level Recovery scope.
 * @return NEON_OK, NEON_INVALID_PARAM for an unknown level, NEON_ERROR if not running, or a backend status.
 */
neon_status_t neon_isp_recover(neon_isp_handle_t *handle, neon_isp_recover_level_t level);

/**
 * @brief Check a tuning profile without a handle and without hardware, for example while a firmware image
 * or a file is being accepted.
 * @details Every entry must name a register of the chip that a profile may write. Registers that the HAL
 * programs itself (geometry, Bayer order, input depth, DMA, buffers, interrupts, output format and size,
 * context ids) are counted as skipped: the HAL's value wins. Read-only registers, registers with
 * write-one-to-clear status bits and offsets that are not registers make the whole profile invalid.
 * @param[in] profile Binary profile (see neon_isp_profile_info_t). Any alignment.
 * @param[in] size_bytes Its size: a non-zero multiple of NEON_ISP_PROFILE_ENTRY_BYTES.
 * @param[out] info Counts; may be NULL.
 * @return NEON_OK, or NEON_INVALID_PARAM for a NULL profile, a bad size or an entry that is refused.
 */
neon_status_t neon_isp_check_profile(const void *profile, uint32_t size_bytes, neon_isp_profile_info_t *info);

/**
 * @brief Write a tuning profile into a stopped ISP.
 * @details The profile is checked first and nothing is written if any entry is refused. A stopped ISP is
 * the only frame-safe moment for every block (MAS 9.5.3: the gamma table is a live RAM and tears when it is
 * reloaded across a frame boundary). The handle returns to idle and frames queued and not reclaimed are
 * forgotten, as by neon_isp_configure(): configure again before neon_isp_start(), so that the fields the HAL
 * owns inside tuning registers (the statistics enables of AEC_CTRL, AWB_CTRL and AF_CTRL) are set by the
 * configuration and not left as the profile wrote them. The profile's white balance gains are kept as the
 * starting point until neon_isp_apply_dynamic_controls().
 * @param[in,out] handle ISP handle, not running.
 * @param[in] profile Binary profile.
 * @param[in] size_bytes Its size.
 * @param[out] info Counts; may be NULL.
 * @return NEON_OK, NEON_BUSY while running, NEON_INVALID_PARAM for a refused profile (nothing written), or a
 * backend status (the profile may be partly written; load it again).
 */
neon_status_t neon_isp_load_profile(neon_isp_handle_t *handle, const void *profile, uint32_t size_bytes,
                                    neon_isp_profile_info_t *info);

/**
 * @brief Read the state, the buffer ownership and the counters.
 * @param[in,out] handle ISP handle.
 * @param[out] status Snapshot.
 * @return NEON_OK on success, or a backend status.
 */
neon_status_t neon_isp_get_status(neon_isp_handle_t *handle, neon_isp_status_t *status);

/**
 * @brief Read the sticky event flags, including events that are masked.
 * @param[in,out] handle ISP handle.
 * @param[out] events NEON_ISP_EVENT_* bits.
 * @return NEON_OK on success, or a backend status.
 */
neon_status_t neon_isp_get_events(neon_isp_handle_t *handle, uint32_t *events);

/**
 * @brief Clear exactly the events that were serviced. Never clear more: an event that arrived while the
 * handler ran would be lost.
 * @param[in,out] handle ISP handle.
 * @param[in] events Bits to clear.
 * @return NEON_OK on success, or a backend status.
 */
neon_status_t neon_isp_clear_events(neon_isp_handle_t *handle, uint32_t events);

/**
 * @brief Change the events that may assert the interrupt line.
 * @param[in,out] handle ISP handle, configured or running.
 * @param[in] event_mask NEON_ISP_EVENT_* bits.
 * @return NEON_OK on success, NEON_INVALID_PARAM for an event the chip cannot raise, NEON_ERROR if not
 * configured, or a backend status.
 */
neon_status_t neon_isp_set_event_mask(neon_isp_handle_t *handle, uint32_t event_mask);

/**
 * @brief Hand a RAW frame to the input DMA. The frame takes the next slot of the rotation, whose order is
 * the order of the calls. May be called before neon_isp_start().
 * @param[in,out] handle ISP handle, configured or running.
 * @param[in] input Frame: address, size, tag and timestamp. The frame must stay untouched until
 * neon_isp_reclaim_input() returns it.
 * @return NEON_OK, NEON_BUSY when every slot is queued, NEON_INVALID_PARAM for a misaligned or too small
 * buffer, or NEON_ERROR if not configured.
 */
neon_status_t neon_isp_queue_input(neon_isp_handle_t *handle, const neon_isp_input_t *input);

/**
 * @brief Take back the oldest input that the DMA has finished with, so that its buffer can be reused.
 * @param[in,out] handle ISP handle.
 * @param[out] input The frame as it was queued, with processed telling whether it went through the
 * pipeline. After stop or recover every frame still queued is returned, not processed.
 * @return NEON_OK, NEON_BUSY if the oldest queued frame is still being read or none is queued, or
 * NEON_ERROR if not configured.
 */
neon_status_t neon_isp_reclaim_input(neon_isp_handle_t *handle, neon_isp_input_t *input);

/**
 * @brief Take the oldest completed NV12 frame that software does not yet hold.
 * @param[in,out] handle ISP handle, running.
 * @param[out] output Planes, strides, tag and timestamps. The buffer is software owned until
 * neon_isp_release_output().
 * @return NEON_OK, NEON_BUSY if no frame has completed, NEON_ERROR if not running.
 */
neon_status_t neon_isp_acquire_output(neon_isp_handle_t *handle, neon_isp_output_t *output);

/**
 * @brief Return an output buffer to the DMA. The completion flag is cleared before the buffer is made
 * free, in that order, so that the DMA never overwrites a frame that a consumer still owns.
 * @param[in,out] handle ISP handle, running.
 * @param[in] buffer_index Buffer returned by neon_isp_acquire_output().
 * @return NEON_OK, NEON_INVALID_PARAM if software does not hold that buffer, NEON_ERROR if not running.
 */
neon_status_t neon_isp_release_output(neon_isp_handle_t *handle, uint8_t buffer_index);

/**
 * @brief Read the newest global statistics, without touching the events.
 * @param[in,out] handle ISP handle, running.
 * @param[out] statistics Results of the enabled generators.
 * @return NEON_OK when at least one generator published a frame that an earlier call did not return,
 * NEON_BUSY when there is nothing new, NEON_ERROR if not running.
 */
neon_status_t neon_isp_read_statistics(neon_isp_handle_t *handle, neon_isp_statistics_t *statistics);

/**
 * @brief Set the tag that the statistics of the next frames carry. Write the tag that identifies the
 * exposure and gain just programmed into the sensor: it comes back with the results, which is how the 3A
 * loop knows which of its settings produced them.
 * @param[in,out] handle ISP handle, configured or running.
 * @param[in] context_id Software tag.
 * @return NEON_OK on success, NEON_ERROR if not configured, or a backend status.
 */
neon_status_t neon_isp_set_context_id(neon_isp_handle_t *handle, uint32_t context_id);

/**
 * @brief Program controls that the pipeline samples at the next start of frame.
 * @param[in,out] handle ISP handle, running.
 * @param[in] controls Gains. A gain of zero is refused: the hardware would black out the channel.
 * @param[out] effective_frame_id Frame id (as in the statistics) by which the controls are in use: the
 * frame counter is read after the write, so a frame that starts in between makes this one later than the
 * first frame that really uses them, never earlier. May be NULL. The caller compares it with
 * controls->target_frame_id.
 * @return NEON_OK, NEON_INVALID_PARAM for a gain outside 1 to caps.max_wb_gain_q8, NEON_ERROR if not running.
 */
neon_status_t neon_isp_apply_dynamic_controls(neon_isp_handle_t *handle,
                                              const neon_isp_dynamic_controls_t *controls,
                                              uint32_t *effective_frame_id);

#ifdef __cplusplus
}
#endif

#endif /* __NEON_ISP_H__ */
