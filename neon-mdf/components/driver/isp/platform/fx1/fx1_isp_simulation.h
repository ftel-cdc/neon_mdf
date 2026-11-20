/**
 * @file fx1_isp_simulation.h
 * @brief Test hooks of the in-RAM ISP backend.
 * @details
 * Models the memory-to-memory DMA of MAS v1.0 chapter 6.25: an input rotation whose slots are valid or
 * empty, an output rotation whose slots are free or done, one frame per step in rotation order, the
 * statistics of a synthetic scene published with a frame id and the context id latched at its start, and
 * the sticky events. No pixel is read or written: the sim only tracks ownership and metadata, so a consumer
 * that needs pixels fills the DDR area itself. Not part of the production API.
 */

#ifndef __FX1_ISP_SIMULATION_H__
#define __FX1_ISP_SIMULATION_H__

/*==================================================================================================
*                                          INCLUDE FILES                                          
==================================================================================================*/
#include "isp/neon_isp.h"
#ifdef __cplusplus
extern "C" {
#endif

/*==================================================================================================
*                                  STRUCTURES AND OTHER TYPEDEFS                                  
==================================================================================================*/
/** What the frames contain, as the statistics see it. Values are on a 0 to 255 scale (12-bit sample >> 4). */
typedef struct {
    uint8_t luma;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint32_t focus;
    uint32_t under_count;        /* Samples below the under-exposure threshold, per frame. */
    uint32_t over_count;
} fx1_isp_sim_scene_t;

typedef enum {
    FX1_ISP_SIM_OP_CONFIGURE = 0,
    FX1_ISP_SIM_OP_START = 1,
    FX1_ISP_SIM_OP_STOP = 2,
    FX1_ISP_SIM_OP_SOFT_RESET = 3
} fx1_isp_sim_op_t;

/** Called by the backend at the start of each operation, before it acts or fails. */
typedef void (*fx1_isp_sim_observer_t)(void *context, fx1_isp_sim_op_t operation);

typedef struct {
    uint32_t configures;
    uint32_t starts;
    uint32_t stops;
    uint32_t soft_resets;
    uint32_t controls_written;
    uint32_t context_writes;
    uint32_t profile_writes;           /* Register writes of tuning profiles since init. */
    uint32_t profile_hash;             /* FNV-1a over each written offset and value, little endian, in order. */
} fx1_isp_sim_counters_t;

/*==================================================================================================
*                                       FUNCTION PROTOTYPES                                       
==================================================================================================*/
/** Replace the scene that the next frames are measured on. The default is luma 96, RGB 88/96/112, focus 0. */
void fx1_isp_sim_set_scene(neon_isp_handle_t *handle, const fx1_isp_sim_scene_t *scene);
/**
 * @brief The DMA and the pipeline process frames, one per step, until nothing can proceed.
 * @details A step needs the pipeline and both DMA engines enabled, the head input slot valid and the head
 * output slot free. A missing input raises IDMA_UNDERRUN and a missing output slot raises ODMA_OVERFLOW,
 * and neither consumes anything. A pending injected AXI error aborts one step instead.
 * @param[in,out] handle ISP handle.
 * @param[in] max_frames Most frames to process.
 * @return Frames processed.
 */
uint32_t fx1_isp_sim_process(neon_isp_handle_t *handle, uint32_t max_frames);
/** The next step is aborted by an AXI response error, as the MAS describes: the rotation does not advance. */
void fx1_isp_sim_inject_axi_error(neon_isp_handle_t *handle, bool write);
/** Raise sticky events as the hardware would. */
void fx1_isp_sim_raise_events(neon_isp_handle_t *handle, uint32_t events);
/** Make the named backend operation fail, to exercise the rollback and retry paths. */
void fx1_isp_sim_set_start_fails(neon_isp_handle_t *handle, bool fails);
void fx1_isp_sim_set_stop_fails(neon_isp_handle_t *handle, bool fails);
void fx1_isp_sim_set_soft_reset_fails(neon_isp_handle_t *handle, bool fails);
void fx1_isp_sim_set_observer(neon_isp_handle_t *handle, fx1_isp_sim_observer_t observer, void *context);
void fx1_isp_sim_get_counters(const neon_isp_handle_t *handle, fx1_isp_sim_counters_t *counters);
/**
 * White balance gains that frames use now, and the frame id of the first frame that used them (0 until a
 * frame has started with controls written).
 */
void fx1_isp_sim_get_applied_controls(const neon_isp_handle_t *handle, uint16_t *red_gain_q8,
                                      uint16_t *blue_gain_q8, uint32_t *first_frame_id);
/**
 * Hold the published results of some generators (bit 0 auto exposure, bit 1 white balance, bit 2 focus):
 * frames go on, but those results keep the frame they had. On the chip each generator publishes on its own
 * completion, so software can see one generator a frame ahead of another.
 */
void fx1_isp_sim_hold_statistics(neon_isp_handle_t *handle, uint8_t generator_mask);
/** Context id written last. */
uint32_t fx1_isp_sim_get_context_id(const neon_isp_handle_t *handle);
/** Event mask last written to the backend. */
uint32_t fx1_isp_sim_get_event_mask(const neon_isp_handle_t *handle);

#ifdef __cplusplus
}
#endif

#endif /* __FX1_ISP_SIMULATION_H__ */
