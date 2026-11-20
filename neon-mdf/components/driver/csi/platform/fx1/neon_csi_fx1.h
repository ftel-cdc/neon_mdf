/**
 * @file neon_csi_fx1.h
 * @brief FX1 extensions of the CSI-2 receiver.
 * @details Settings that only the FX1 MIPI CSI-2 controller has (MAS v1.4). Each extended struct holds
 * its generic struct of neon_csi.h as first member.
 *
 * Notes for FX1 applications:
 * - Every neon_csi_fx1_set_*() writes the registers directly. Call it after neon_csi_configure(), which
 *   writes the defaults back, and before neon_csi_start().
 * - One buffer holds RAW VC0, RAW VC1, metadata VC0 and metadata VC1, in that order: 2 x
 *   (dma.buffer_size_bytes + metadata.area_size_bytes). The VC1 regions are reserved even with one
 *   exposure. Allocate layout.buffer_bytes, and take each region from frame.pixel_addr[] / meta_addr[].
 * - Buffer addresses, dma.buffer_size_bytes, metadata.area_size_bytes and the line pitch are multiples of
 *   8 bytes; the hardware ignores bits 2:0.
 * - dma.line_pitch_bytes is bytes_per_line plus the padding after each line. The LINE_PITCH register holds
 *   the two apart, so the padding is at most 255 bytes.
 * - The metadata area holds the record at offset 0x00 and the embedded payload from 0x80: area_size_bytes
 *   is at least 0x80 plus the largest embedded payload. metadata.data_type is never 0x10, 0x11, 0x2B or 0x2C.
 * - Only VC0 and VC1 are received; packets on other virtual channels are ignored.
 * - frame.vc[n].error comes from err_summary of the metadata record: it is always false while metadata
 *   capture is off. NEON_CSI_SUB_EVENT_FRAME_ERROR_VC(n) works without metadata.
 * - FX1 has no interrupt for NEON_CSI_SUB_EVENT_FRAME_START or NEON_CSI_SUB_EVENT_PACKET_SHORT: they are
 *   not in caps.supported_sub_events. Poll neon_csi_read_short_packet() instead.
 * - Frame sub events exist for VC0 and VC1 only.
 */

#ifndef __NEON_CSI_FX1_H__
#define __NEON_CSI_FX1_H__

/*==================================================================================================
*                                          INCLUDE FILES
==================================================================================================*/
#include "csi/neon_csi.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==================================================================================================
*                                        DEFINES AND MACROS
==================================================================================================*/
#define NEON_CSI_FX1_BUFFER_COUNT 2U  /* Ping-pong slots of the Buffer Manager; reported as caps.buffer_count. */
#define NEON_CSI_FX1_VC_COUNT 2U      /* VC0 and VC1 with their own frame context; reported as caps.vc_count. */
#define NEON_CSI_FX1_EXTRACT_SLOTS 2U /* Fields the Embedded Data Path can extract per frame (N_EXTRACT). */

/* Sub events of NEON_CSI_EVENT_PLATFORM on FX1. */
#define NEON_CSI_FX1_SUB_EVENT_CLOCK_STOP_STATE       (1UL << 0) /* The clock lane entered Stop State (IRQ_STATUS.STOP_STATE). */
#define NEON_CSI_FX1_SUB_EVENT_LINE_BUFFER_FULL       (1UL << 1) /* The Line Buffer reached its watermark (LINE_BUFFER_FULL). */
#define NEON_CSI_FX1_SUB_EVENT_LANE_ABORT             (1UL << 2) /* Lane Management discarded a burst (LANE_ALIGN_ABORT). */
#define NEON_CSI_FX1_SUB_EVENT_WC_ALIGN_ERROR         (1UL << 3) /* A line ended inside a pixel-packing unit (ERR_WC_ALIGN). */
#define NEON_CSI_FX1_SUB_EVENT_FORMAT_BRIDGE_OVERFLOW (1UL << 4) /* A Format Bridge output beat was lost (FMTBRIDGE_OVERFLOW). */
#define NEON_CSI_FX1_SUB_EVENT_STREAM_FRAME_RECEIVED  (1UL << 5) /* A capture was delivered on the AXI4-Stream path (FRAME_RECEIVED). */
#define NEON_CSI_FX1_SUB_EVENT_RESOLUTION_MISMATCH    (1UL << 6) /* A frame differed from config.width or height (RESOLUTION_MISMATCH). */
#define NEON_CSI_FX1_SUB_EVENT_FRAME_NUMBER_ERROR     (1UL << 7) /* The Frame Start number did not advance by one (FRAME_NUM_SEQ_ERR); needs frame_number_check. */

/*==================================================================================================
*                                  STRUCTURES AND OTHER TYPEDEFS
==================================================================================================*/
/** What the DMA does with a buffer whose frame had an error. Values are DMA_CONTROL.err_frame_policy. */
typedef enum {
    NEON_CSI_FX1_ERROR_POLICY_DISCARD = 0,            /* The buffer is freed and written again; not reported. */
    NEON_CSI_FX1_ERROR_POLICY_RELEASE_WITH_ERROR = 1  /* Handed to software as NEON_CSI_BUFFER_SW_ERROR. Reset value, recommended. */
} neon_csi_fx1_error_policy_t;

/** FX1 DMA configuration. */
typedef struct {
    neon_csi_dma_config_t base;                /* Generic part; must stay first. */
    neon_csi_fx1_error_policy_t error_policy;  /* Sampled at each buffer claim. */
} neon_csi_fx1_dma_config_t;

/** Test patterns. Values are TPG_CONTROL.pattern. */
typedef enum {
    NEON_CSI_FX1_TPG_VERTICAL_BARS = 0,        /* Colour bars across the width. */
    NEON_CSI_FX1_TPG_HORIZONTAL_BARS = 1       /* Colour bars down the height. */
} neon_csi_fx1_tpg_pattern_t;

/** Bayer order of the test pattern, named from its first two lines. Values are TPG_CONTROL.tpg_cfa_order. */
typedef enum {
    NEON_CSI_FX1_CFA_RGGB = 0,                 /* R site at even row, even column. */
    NEON_CSI_FX1_CFA_GRBG = 1,                 /* R site at even row, odd column. */
    NEON_CSI_FX1_CFA_GBRG = 2,                 /* R site at odd row, even column. */
    NEON_CSI_FX1_CFA_BGGR = 3                  /* R site at odd row, odd column. */
} neon_csi_fx1_cfa_order_t;

/**
 * FX1 test pattern generator. Only RAW10 and RAW12 can be generated.
 * base.width is rounded down to a multiple of 4 and must be at least 8. base.height counts the lines of
 * each virtual channel, at least 8: with dual_vc the generator sends twice as many lines in all.
 * Do not let the sensor stream while the source switches between the sensor and the generator.
 */
typedef struct {
    neon_csi_tpg_config_t base;                /* Generic part; must stay first. pattern is a neon_csi_fx1_tpg_pattern_t. */
    neon_csi_fx1_cfa_order_t cfa_order;        /* Bayer order of the pattern. */
    bool dual_vc;                              /* Interleave VC0 (long) then VC1 (short) per line; false: VC0 only. */
    uint16_t hblank_cycles;                    /* sys_clk cycles after each line; 0 sends lines back to back. */
    uint16_t vblank_lines;                     /* Blank line-times after each frame; 0 sends frames back to back. */
} neon_csi_fx1_tpg_config_t;

/** One field to extract from the embedded lines (EMB_EXTRACT_CFGm). Shared by both virtual channels. */
typedef struct {
    uint8_t line_index;                        /* Embedded line of the frame that holds the field, from 0. */
    uint16_t byte_offset;                      /* Offset of its first byte in that line. */
    uint8_t length_bytes;                      /* Field length, 1 to 4; the first byte lands in bits 7:0. */
} neon_csi_fx1_extract_slot_t;

/** FX1 metadata capture. */
typedef struct {
    neon_csi_metadata_config_t base;           /* Generic part; must stay first. */
    bool frame_number_check;                   /* Check that each Frame Start number is the previous + 1 (FNUM_CHECK_EN). */
    bool extract_enable;                       /* Run the field extractor (EMB_EXTRACT_EN); the lines are written either way. */
    neon_csi_fx1_extract_slot_t extract[NEON_CSI_FX1_EXTRACT_SLOTS]; /* Fields to extract; used with extract_enable. */
} neon_csi_fx1_metadata_config_t;

/** FX1 receiver snapshot. The extra counters are 16 bits wide and saturate at 0xFFFF. */
typedef struct {
    neon_csi_status_t base;                    /* Generic part; must stay first. */
    uint16_t lane_abort_count;                 /* Bursts discarded by Lane Management, LANE_ABORT_COUNT. */
    uint16_t axi_slverr_count;                 /* DMA writes answered SLVERR, AXI_ERROR_COUNT.BRESP_SLVERR_COUNT. */
    uint16_t axi_decerr_count;                 /* DMA writes answered DECERR, AXI_ERROR_COUNT.BRESP_DECERR_COUNT. */
} neon_csi_fx1_status_t;

/*==================================================================================================
*                                       FUNCTION PROTOTYPES
==================================================================================================*/
/**
 * @brief Program the DMA with its FX1 settings, in place of config.dma. Optional: without it the DMA uses
 * config.dma and NEON_CSI_FX1_ERROR_POLICY_RELEASE_WITH_ERROR.
 * @param[in,out] handle Receiver handle, configured by neon_csi_configure() and not running.
 * @param[in] dma FX1 DMA configuration.
 * @return NEON_OK on success, NEON_INVALID_PARAM for a NULL pointer or an invalid value, NEON_BUSY while
 * running, NEON_ERROR if not configured.
 */
neon_status_t neon_csi_fx1_set_dma_config(neon_csi_handle_t *handle, const neon_csi_fx1_dma_config_t *dma);

/**
 * @brief Change the FX1 settings of the test pattern generator. Optional: neon_csi_configure() already
 * programs config.test_pattern with the FX1 defaults (RGGB, VC0 only, no blanking).
 * @param[in,out] handle Receiver handle, configured by neon_csi_configure() and not running.
 * @param[in] tpg FX1 test pattern configuration. base.enable, base.width and base.height must equal
 * config.test_pattern, since the buffers were sized from them; base.pattern may change.
 * @return NEON_OK on success, NEON_INVALID_PARAM for a NULL pointer, an invalid value or a different
 * geometry, NEON_BUSY while running, NEON_ERROR if not configured.
 */
neon_status_t neon_csi_fx1_set_tpg_config(neon_csi_handle_t *handle, const neon_csi_fx1_tpg_config_t *tpg);

/**
 * @brief Read the generic snapshot of neon_csi_get_status() plus the FX1 counters.
 * @param[in,out] handle Receiver handle.
 * @param[out] status FX1 snapshot.
 * @return NEON_OK on success, NEON_INVALID_PARAM for a NULL pointer, NEON_ERROR if not initialized.
 */
neon_status_t neon_csi_fx1_get_status(neon_csi_handle_t *handle, neon_csi_fx1_status_t *status);

/**
 * @brief Set the software tag that the next captures carry in their metadata record (METADATA_RECORD_ID).
 * The value is sampled when a capture opens, so it may be changed while running; a capture already open
 * keeps the previous one.
 * @param[in,out] handle Receiver handle, configured.
 * @param[in] record_id Software tag, any value.
 * @return NEON_OK on success, NEON_INVALID_PARAM for a NULL pointer, NEON_ERROR if not configured.
 */
neon_status_t neon_csi_fx1_set_record_id(neon_csi_handle_t *handle, uint32_t record_id);

/**
 * @brief Choose the lead virtual channel (PROTOCOL_CONFIG.vc_exposure_map). Its Frame Start opens a capture;
 * the other channel joins that capture at its next Frame Start and shares its frame_id, timestamp and
 * record_id. Set it to the channel the sensor starts first (staggered HDR: the long exposure), or the
 * pairs are formed across two exposures. Without it the lead channel is VC0. With the test pattern
 * generator enabled the lead channel must be VC0 (MAS 9.1): vc 1 is then rejected.
 * @param[in,out] handle Receiver handle, configured by neon_csi_configure() and not running.
 * @param[in] vc Lead virtual channel, 0 or 1.
 * @return NEON_OK on success, NEON_INVALID_PARAM for a NULL pointer or vc above 1, NEON_BUSY while running,
 * NEON_ERROR if not configured.
 */
neon_status_t neon_csi_fx1_set_lead_vc(neon_csi_handle_t *handle, uint8_t vc);

/**
 * @brief Program the metadata capture with its FX1 settings, in place of config.metadata. Optional:
 * without it the frame-number check and the extractor are off. The field offsets can be read first from
 * the embedded lines in memory, which are written while the extractor is off.
 * @param[in,out] handle Receiver handle, configured by neon_csi_configure() and not running.
 * @param[in] metadata FX1 metadata configuration.
 * @return NEON_OK on success, NEON_INVALID_PARAM for a NULL pointer or an invalid value, NEON_BUSY while
 * running, NEON_ERROR if not configured.
 */
neon_status_t neon_csi_fx1_set_metadata_config(neon_csi_handle_t *handle,
                                               const neon_csi_fx1_metadata_config_t *metadata);

/**
 * @brief Read the fields extracted from one frame, from its metadata record (words 7 and 8).
 * @param[in] frame Frame from neon_csi_acquire_frame(), still held.
 * @param[in] vc Virtual channel, with its vc_mask bit set in frame.
 * @param[out] values Extracted value of each slot, right-aligned.
 * @return NEON_OK on success, NEON_INVALID_PARAM for a NULL pointer, a channel not in the frame or a frame
 * without metadata.
 */
neon_status_t neon_csi_fx1_get_extracted(const neon_csi_frame_t *frame, uint8_t vc,
                                         uint32_t values[NEON_CSI_FX1_EXTRACT_SLOTS]);

#ifdef __cplusplus
}
#endif

#endif /* __NEON_CSI_FX1_H__ */
