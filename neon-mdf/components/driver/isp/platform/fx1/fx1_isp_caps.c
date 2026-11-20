/**
 * @file fx1_isp_caps.c
 * @brief What the FX1 ISP provides (MAS v1.0, register map 1.07). Facts of the IP, shared by every FX1 backend.
 * @details
 * Limits that are register field widths are read from the generated register header, which comes from
 * FX1.yaml, so that they follow the register map. Build parameters of the IP and sizes that the register
 * description states only in words are written here.
 */

/*==================================================================================================
*                                          INCLUDE FILES                                          
==================================================================================================*/
#include "isp/neon_isp.h"
#include "isp/platform/common/neon_isp_hw.h"
#include "fx1_registers.h"

/*==================================================================================================
*                                        DEFINES AND MACROS                                       
==================================================================================================*/
/* Build parameters of the IP (MAS 5): not registers, so not in FX1.yaml. */
#define FX1_ISP_INPUT_BUFFERS 4U           /* PARA_NUM_DMA_BUFFERS, each DMA engine. */
#define FX1_ISP_OUTPUT_BUFFERS 4U
#define FX1_ISP_AEC_HIST_BINS 64U          /* MAS 9.6. */
/* External frame size of COMMON_FRAME_WIDTH and COMMON_FRAME_HEIGHT, before the Bayer phase trim. */
#define FX1_ISP_MIN_WIDTH 32U
#define FX1_ISP_MAX_WIDTH 3842U
#define FX1_ISP_MIN_HEIGHT 32U
#define FX1_ISP_MAX_HEIGHT 2162U
#define FX1_ISP_SIZE_GRANULARITY 2U        /* Even width and height (MAS 6.25.3.4). */
/* The AXI data port is 128 bits wide by default (PARA_AXI_DATA_WIDTH): base and stride in multiples of it. */
#ifndef FX1_ISP_DMA_ALIGN_BYTES
#define FX1_ISP_DMA_ALIGN_BYTES 16UL
#endif
/* Limits that follow from a register field width. */
#define FX1_ISP_MAX_BURST_BEATS (1UL << NEON_FX1_ISP_DMA_CTRL_MAX_BURST_M1_WIDTH)
#define FX1_ISP_MAX_WB_GAIN_Q8 ((1UL << NEON_FX1_ISP_WB_GAIN_R_GAIN_R_WIDTH) - 1UL)
/* NEON_ISP_EVENT_* bits 0 to 15; the backend translates the status registers to them explicitly. */
#define FX1_ISP_EVENTS 0x0000FFFFUL

_Static_assert(FX1_ISP_INPUT_BUFFERS <= NEON_ISP_MAX_BUFFERS, "raise NEON_ISP_MAX_BUFFERS");
_Static_assert(FX1_ISP_OUTPUT_BUFFERS <= NEON_ISP_MAX_BUFFERS, "raise NEON_ISP_MAX_BUFFERS");
_Static_assert(FX1_ISP_AEC_HIST_BINS <= NEON_ISP_MAX_HIST_BINS, "raise NEON_ISP_MAX_HIST_BINS");
_Static_assert(FX1_ISP_INPUT_BUFFERS <= NEON_FX1_ISP_IDMA_BUF_VALID_IDMA_BUF_VALID_WIDTH,
               "IDMA_BUF_VALID has fewer bits than the input rotation");
_Static_assert(FX1_ISP_OUTPUT_BUFFERS <= NEON_FX1_ISP_ODMA_BUF_FREE_ODMA_BUF_FREE_WIDTH,
               "ODMA_BUF_FREE has fewer bits than the output rotation");
_Static_assert(FX1_ISP_OUTPUT_BUFFERS <= NEON_FX1_ISP_ODMA_BUF_DONE_ODMA_BUF_DONE_WIDTH,
               "ODMA_BUF_DONE has fewer bits than the output rotation");

/*==================================================================================================
*                                         GLOBAL FUNCTIONS                                        
==================================================================================================*/
neon_status_t neon_isp_hw_get_caps(neon_isp_caps_t *caps)
{
    caps->input_buffers = FX1_ISP_INPUT_BUFFERS;
    caps->output_buffers = FX1_ISP_OUTPUT_BUFFERS;
    caps->aec_hist_bins = FX1_ISP_AEC_HIST_BINS;
    caps->aec = true;
    caps->awb = true;
    caps->af = true;
    caps->raw_depth_mask = (uint16_t)((1U << (unsigned)NEON_ISP_RAW10) | (1U << (unsigned)NEON_ISP_RAW12));
    caps->bayer_pattern_mask = 0x0FU; /* COMMON_BAYER.PATTERN is two bits wide: all four patterns. */
    caps->min_width = FX1_ISP_MIN_WIDTH;
    caps->max_width = FX1_ISP_MAX_WIDTH;
    caps->min_height = FX1_ISP_MIN_HEIGHT;
    caps->max_height = FX1_ISP_MAX_HEIGHT;
    caps->size_granularity = FX1_ISP_SIZE_GRANULARITY;
    caps->max_burst_beats = (uint16_t)FX1_ISP_MAX_BURST_BEATS;
    caps->dma_align_bytes = FX1_ISP_DMA_ALIGN_BYTES;
    caps->max_wb_gain_q8 = (uint16_t)FX1_ISP_MAX_WB_GAIN_Q8;
    caps->supported_events = FX1_ISP_EVENTS;
    return NEON_OK;
}
