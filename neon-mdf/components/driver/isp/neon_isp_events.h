/**
 * @file neon_isp_events.h
 * @brief Event bits reported by the ISP.
 * @details
 * Kept apart from neon_isp.h so that the main header holds only the contract of the calls. Callers need
 * not include this file themselves: neon_isp.h does. The bits are a logical numbering owned by the HAL,
 * not the numbering of any chip's interrupt registers. Which of them a chip can raise is
 * neon_isp_caps_t.supported_events, and a backend translates its own status registers to these bits
 * explicitly. Every bit is sticky and cleared by writing it back (ISP MAS 9.3).
 */

#ifndef __NEON_ISP_EVENTS_H__
#define __NEON_ISP_EVENTS_H__

#ifdef __cplusplus
extern "C" {
#endif

/*==================================================================================================
*                                        DEFINES AND MACROS                                       
==================================================================================================*/
/* Pipeline group (COMMON_IRQ_STATUS). */
#define NEON_ISP_EVENT_FRAME_DONE (1UL << 0)      /* A frame left the pixel pipeline. */
#define NEON_ISP_EVENT_PIPELINE_ERROR (1UL << 1) /* A processing block reported an error; see the block bits. */
#define NEON_ISP_EVENT_STATS_AEC (1UL << 2)      /* A new auto-exposure result is published. */
#define NEON_ISP_EVENT_STATS_AWB (1UL << 3)      /* A new auto-white-balance result is published. */
#define NEON_ISP_EVENT_STATS_AF (1UL << 4)       /* A new auto-focus result is published. */
/* DMA group (DMA_IRQ_STAT). */
#define NEON_ISP_EVENT_IDMA_START (1UL << 5)     /* The input DMA began a frame. */
#define NEON_ISP_EVENT_IDMA_DONE (1UL << 6)      /* The input DMA read the last sample: the input buffer is free. */
#define NEON_ISP_EVENT_IDMA_UNDERRUN (1UL << 7)  /* Enabled and idle, and no input buffer is queued. */
#define NEON_ISP_EVENT_ODMA_DONE (1UL << 8)      /* Both output planes of a frame are in memory. */
#define NEON_ISP_EVENT_ODMA_OVERFLOW (1UL << 9)  /* The pipeline has a frame and no output buffer is free. */
#define NEON_ISP_EVENT_AXI_READ_ERROR (1UL << 10)
#define NEON_ISP_EVENT_AXI_WRITE_ERROR (1UL << 11)
#define NEON_ISP_EVENT_CONFIG_ERROR (1UL << 12) /* A start was refused: an illegal DMA configuration. */
/* Per-block error bits behind PIPELINE_ERROR. */
#define NEON_ISP_EVENT_LSC_ERROR (1UL << 13)
#define NEON_ISP_EVENT_EE_ERROR (1UL << 14)
#define NEON_ISP_EVENT_BPC_OVERFLOW (1UL << 15)

#ifdef __cplusplus
}
#endif

#endif /* __NEON_ISP_EVENTS_H__ */
