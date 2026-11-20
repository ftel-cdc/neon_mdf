/**
 * @file fx1_isp_simulation.c
 * @brief In-RAM backend of the ISP.
 */

/*==================================================================================================
*                                          INCLUDE FILES                                          
==================================================================================================*/
#include "isp/platform/fx1/fx1_isp_simulation.h"
#include "isp/platform/common/neon_isp_hw.h"
#include <string.h>

/*==================================================================================================
*                                        DEFINES AND MACROS                                       
==================================================================================================*/
#define SIM_AEC 0U
#define SIM_AWB 1U
#define SIM_AF 2U
#define SIM_SOF_CLOCKS 100U
#define SIM_FRAME_CLOCKS 1000U
/* A 12-bit Bayer sample holds the 0 to 255 scale value in its top eight bits (MAS 6.25.3.2.0.1). */
#define SIM_SCALE_SHIFT 4U
#define SIM_HIST_SHIFT 6U

/*==================================================================================================
*                                  STRUCTURES AND OTHER TYPEDEFS                                  
==================================================================================================*/
/* What a generator published for one frame; the values are rebuilt from the scene taken at that time. */
typedef struct {
    bool valid;
    uint32_t frame_id;
    uint32_t context_id;
    fx1_isp_sim_scene_t scene;
    uint32_t pixels;
} sim_result_t;

typedef struct {
    bool configured;
    bool isp_en;
    bool dma_en;
    bool start_fails;
    bool stop_fails;
    bool soft_reset_fails;
    bool axi_error_pending;
    bool axi_error_write;
    bool stats_en[3];
    uint8_t stats_hold;
    uint16_t width;
    uint16_t height;
    uint8_t in_valid;
    uint8_t out_free;
    uint8_t out_done;
    uint8_t in_head;
    uint8_t out_head;
    uint32_t in_tag[NEON_ISP_MAX_BUFFERS];
    uint32_t in_ts[NEON_ISP_MAX_BUFFERS];
    uint32_t out_tag[NEON_ISP_MAX_BUFFERS];
    uint32_t out_ts[NEON_ISP_MAX_BUFFERS];
    uint32_t out_sof[NEON_ISP_MAX_BUFFERS];
    uint32_t out_eof[NEON_ISP_MAX_BUFFERS];
    uint32_t frames_in;
    uint32_t frames_out;
    uint32_t clock;
    uint32_t events;
    uint32_t event_mask;
    uint32_t context_id;
    uint16_t wb_pending_red;
    uint16_t wb_pending_blue;
    bool wb_written;
    uint16_t wb_red;
    uint16_t wb_blue;
    uint32_t wb_first_frame;
    fx1_isp_sim_scene_t scene;
    sim_result_t result[3];
    fx1_isp_sim_observer_t observer;
    void *observer_context;
    fx1_isp_sim_counters_t counters;
} isp_sim_t;

_Static_assert(sizeof(isp_sim_t) <= sizeof(((neon_isp_handle_t *)0)->backend_words),
               "isp_sim_t does not fit in the handle; raise NEON_ISP_BACKEND_WORDS");

/*==================================================================================================
*                                         LOCAL FUNCTIONS                                         
==================================================================================================*/
static isp_sim_t *isp_sim(neon_isp_handle_t *handle)
{
    return (isp_sim_t *)(void *)handle->backend_words;
}

static const isp_sim_t *isp_sim_const(const neon_isp_handle_t *handle)
{
    return (const isp_sim_t *)(const void *)handle->backend_words;
}

static uint8_t sim_mask(uint8_t slot)
{
    return (uint8_t)(1U << slot);
}

static void sim_observe(isp_sim_t *sim, fx1_isp_sim_op_t operation)
{
    if (sim->observer != NULL) {
        sim->observer(sim->observer_context, operation);
    }
}

/* Every output slot becomes free: ODMA_BUF_FREE written with a one for each buffer software allocated. */
static void sim_free_all_outputs(neon_isp_handle_t *handle)
{
    isp_sim_t *sim = isp_sim(handle);

    sim->out_free = (uint8_t)((1U << handle->caps.output_buffers) - 1U);
    sim->out_done = 0U;
    sim->out_head = 0U;
}

static void sim_default_scene(fx1_isp_sim_scene_t *scene)
{
    (void)memset(scene, 0, sizeof(*scene));
    scene->luma = 96U;
    scene->red = 88U;
    scene->green = 96U;
    scene->blue = 112U;
}

static void sim_publish(isp_sim_t *sim, uint8_t block, uint32_t frame_id)
{
    sim_result_t *result = &sim->result[block];

    result->valid = true;
    result->frame_id = frame_id;
    result->context_id = sim->context_id; /* Latched at the start of this frame. */
    result->scene = sim->scene;
    result->pixels = (uint32_t)sim->width * sim->height;
}

/*==================================================================================================
*                                         GLOBAL FUNCTIONS                                        
==================================================================================================*/
neon_status_t neon_isp_hw_init(neon_isp_handle_t *handle)
{
    isp_sim_t *sim = isp_sim(handle);

    (void)memset(sim, 0, sizeof(*sim));
    sim_default_scene(&sim->scene);
    return NEON_OK;
}

/* The sim has no register file: it records what was written and in which order, which is what a test of a
 * profile load needs to see. */
static uint32_t sim_hash_word(uint32_t hash, uint32_t word)
{
    uint32_t i;

    for (i = 0U; i < 4U; i++) {
        hash ^= (word >> (8U * i)) & 0xFFU;
        hash *= 16777619U;
    }
    return hash;
}

neon_status_t neon_isp_hw_profile_write(neon_isp_handle_t *handle, uint32_t offset, uint32_t value)
{
    isp_sim_t *sim = isp_sim(handle);

    if (sim->counters.profile_writes == 0U) {
        sim->counters.profile_hash = 2166136261U;
    }
    sim->counters.profile_writes++;
    sim->counters.profile_hash = sim_hash_word(sim->counters.profile_hash, offset);
    sim->counters.profile_hash = sim_hash_word(sim->counters.profile_hash, value);
    sim->configured = false; /* As on the chip: what configure() programmed is no longer complete. */
    return NEON_OK;
}

neon_status_t neon_isp_hw_configure(neon_isp_handle_t *handle, const neon_isp_config_t *config)
{
    isp_sim_t *sim = isp_sim(handle);

    sim_observe(sim, FX1_ISP_SIM_OP_CONFIGURE);
    sim->counters.configures++;
    sim->configured = true;
    sim->isp_en = false;
    sim->dma_en = false;
    sim->width = config->width;
    sim->height = config->height;
    sim->stats_en[SIM_AEC] = config->stats_aec;
    sim->stats_en[SIM_AWB] = config->stats_awb;
    sim->stats_en[SIM_AF] = config->stats_af;
    sim->event_mask = config->event_mask;
    sim->in_valid = 0U;
    sim->out_free = 0U;
    sim->out_done = 0U;
    sim->in_head = 0U;
    sim->out_head = 0U;
    sim->events = 0U;
    sim->frames_in = 0U;
    sim->frames_out = 0U;
    (void)memset(sim->result, 0, sizeof(sim->result));
    return NEON_OK;
}

neon_status_t neon_isp_hw_start(neon_isp_handle_t *handle)
{
    isp_sim_t *sim = isp_sim(handle);

    sim_observe(sim, FX1_ISP_SIM_OP_START);
    if (sim->start_fails) {
        return NEON_ERROR;
    }
    sim->counters.starts++;
    sim->isp_en = true;          /* ISP_EN first (MAS 9.1.1 step 7), then the buffers and the DMA enables. */
    sim_free_all_outputs(handle);
    sim->in_head = 0U;
    sim->dma_en = true;
    return NEON_OK;
}

neon_status_t neon_isp_hw_stop(neon_isp_handle_t *handle)
{
    isp_sim_t *sim = isp_sim(handle);

    sim_observe(sim, FX1_ISP_SIM_OP_STOP);
    if (sim->stop_fails) {
        return NEON_ERROR;
    }
    sim->counters.stops++;
    sim->dma_en = false;
    sim->isp_en = false;
    sim->in_valid = 0U;
    sim->out_free = 0U;
    sim->out_done = 0U;
    sim->in_head = 0U;
    sim->out_head = 0U;
    return NEON_OK;
}

neon_status_t neon_isp_hw_soft_reset(neon_isp_handle_t *handle)
{
    isp_sim_t *sim = isp_sim(handle);

    sim_observe(sim, FX1_ISP_SIM_OP_SOFT_RESET);
    if (sim->soft_reset_fails) {
        return NEON_ERROR;
    }
    sim->counters.soft_resets++;
    /* The configuration survives. The frame in flight, the ownership bits, the results and the pending events go. */
    sim->in_valid = 0U;
    sim->in_head = 0U;
    sim->events = 0U;
    (void)memset(sim->result, 0, sizeof(sim->result));
    sim_free_all_outputs(handle);
    sim->isp_en = true;
    sim->dma_en = true;
    return NEON_OK;
}

neon_status_t neon_isp_hw_read_events(neon_isp_handle_t *handle, uint32_t *events)
{
    *events = isp_sim(handle)->events;
    return NEON_OK;
}

neon_status_t neon_isp_hw_clear_events(neon_isp_handle_t *handle, uint32_t events)
{
    isp_sim(handle)->events &= ~events;
    return NEON_OK;
}

neon_status_t neon_isp_hw_set_event_mask(neon_isp_handle_t *handle, uint32_t event_mask)
{
    isp_sim(handle)->event_mask = event_mask;
    return NEON_OK;
}

neon_status_t neon_isp_hw_queue_input(neon_isp_handle_t *handle, uint8_t slot, const neon_isp_input_t *input)
{
    isp_sim_t *sim = isp_sim(handle);

    sim->in_tag[slot] = input->tag;
    sim->in_ts[slot] = input->timestamp;
    sim->in_valid |= sim_mask(slot); /* Address, tag and timestamp first, the valid bit last. */
    return NEON_OK;
}

neon_status_t neon_isp_hw_read_buffers(neon_isp_handle_t *handle, uint8_t *input_valid_mask, uint8_t *output_done_mask)
{
    const isp_sim_t *sim = isp_sim(handle);

    *input_valid_mask = sim->in_valid;
    *output_done_mask = sim->out_done;
    return NEON_OK;
}

neon_status_t neon_isp_hw_read_output(neon_isp_handle_t *handle, uint8_t slot, neon_isp_output_t *output)
{
    const isp_sim_t *sim = isp_sim(handle);

    output->tag = sim->out_tag[slot];
    output->timestamp = sim->out_ts[slot];
    output->sof_count = sim->out_sof[slot];
    output->eof_count = sim->out_eof[slot];
    return NEON_OK;
}

neon_status_t neon_isp_hw_release_output(neon_isp_handle_t *handle, uint8_t slot)
{
    isp_sim_t *sim = isp_sim(handle);

    sim->out_done &= (uint8_t)~sim_mask(slot); /* Clear done first, then set free: never the other way round. */
    sim->out_free |= sim_mask(slot);
    return NEON_OK;
}

neon_status_t neon_isp_hw_read_statistics(neon_isp_handle_t *handle, neon_isp_statistics_t *statistics)
{
    const isp_sim_t *sim = isp_sim_const(handle);
    uint32_t i;

    if (sim->result[SIM_AEC].valid) {
        const sim_result_t *r = &sim->result[SIM_AEC];
        const uint64_t sample = (uint64_t)r->scene.luma << SIM_SCALE_SHIFT;

        statistics->aec.valid = true;
        statistics->aec.frame_id = r->frame_id;
        statistics->aec.context_id = r->context_id;
        statistics->aec.count = r->pixels;
        statistics->aec.sum = sample * r->pixels;
        statistics->aec.under_count = r->scene.under_count;
        statistics->aec.over_count = r->scene.over_count;
        statistics->aec.histogram_valid = true;
        for (i = 0U; i < handle->caps.aec_hist_bins; i++) {
            statistics->aec.histogram[i] = 0U;
        }
        statistics->aec.histogram[sample >> SIM_HIST_SHIFT] = r->pixels;
    }
    if (sim->result[SIM_AWB].valid) {
        const sim_result_t *r = &sim->result[SIM_AWB];
        const uint32_t cells = r->pixels / 4U; /* One co-sited R, G, B triple per 2x2 Bayer cell. */

        statistics->awb.valid = true;
        statistics->awb.frame_id = r->frame_id;
        statistics->awb.context_id = r->context_id;
        statistics->awb.count = cells;
        statistics->awb.sum_r = ((uint64_t)r->scene.red << SIM_SCALE_SHIFT) * cells;
        statistics->awb.sum_g = ((uint64_t)r->scene.green << SIM_SCALE_SHIFT) * cells;
        statistics->awb.sum_b = ((uint64_t)r->scene.blue << SIM_SCALE_SHIFT) * cells;
    }
    if (sim->result[SIM_AF].valid) {
        const sim_result_t *r = &sim->result[SIM_AF];

        statistics->af.valid = true;
        statistics->af.frame_id = r->frame_id;
        statistics->af.context_id = r->context_id;
        statistics->af.focus = r->scene.focus;
    }
    return NEON_OK;
}

neon_status_t neon_isp_hw_set_context_id(neon_isp_handle_t *handle, uint32_t context_id)
{
    isp_sim_t *sim = isp_sim(handle);

    sim->context_id = context_id;
    sim->counters.context_writes++;
    return NEON_OK;
}

neon_status_t neon_isp_hw_apply_controls(neon_isp_handle_t *handle, const neon_isp_dynamic_controls_t *controls)
{
    isp_sim_t *sim = isp_sim(handle);

    sim->wb_pending_red = controls->wb_red_gain_q8;
    sim->wb_pending_blue = controls->wb_blue_gain_q8;
    sim->wb_written = true; /* Sampled at the next start of frame. */
    sim->counters.controls_written++;
    return NEON_OK;
}

neon_status_t neon_isp_hw_read_counters(neon_isp_handle_t *handle, neon_isp_status_t *status)
{
    const isp_sim_t *sim = isp_sim(handle);

    status->frames_in = sim->frames_in;
    status->frames_out = sim->frames_out;
    return NEON_OK;
}

void fx1_isp_sim_hold_statistics(neon_isp_handle_t *handle, uint8_t generator_mask)
{
    if (handle != NULL) {
        isp_sim(handle)->stats_hold = generator_mask;
    }
}

void fx1_isp_sim_set_scene(neon_isp_handle_t *handle, const fx1_isp_sim_scene_t *scene)
{
    if (handle != NULL && scene != NULL) {
        isp_sim(handle)->scene = *scene;
    }
}

uint32_t fx1_isp_sim_process(neon_isp_handle_t *handle, uint32_t max_frames)
{
    isp_sim_t *sim;
    uint32_t done = 0U;

    if (handle == NULL) {
        return 0U;
    }
    sim = isp_sim(handle);
    while (done < max_frames && sim->isp_en && sim->dma_en) {
        const uint8_t in_slot = sim->in_head;
        const uint8_t out_slot = sim->out_head;
        uint32_t frame_id;
        uint32_t block;

        if (sim->axi_error_pending) {
            sim->axi_error_pending = false;
            sim->events |= sim->axi_error_write ? NEON_ISP_EVENT_AXI_WRITE_ERROR : NEON_ISP_EVENT_AXI_READ_ERROR;
            sim->events |= NEON_ISP_EVENT_PIPELINE_ERROR;
            break; /* Aborted: nothing consumed, the rotation did not advance. */
        }
        if ((sim->in_valid & sim_mask(in_slot)) == 0U) {
            sim->events |= NEON_ISP_EVENT_IDMA_UNDERRUN;
            break;
        }
        if ((sim->out_free & sim_mask(out_slot)) == 0U) {
            sim->events |= NEON_ISP_EVENT_ODMA_OVERFLOW;
            break;
        }
        sim->frames_in++;
        frame_id = sim->frames_in;
        sim->events |= NEON_ISP_EVENT_IDMA_START;
        if (sim->wb_written) { /* Controls are sampled at the start of a frame. */
            if (sim->wb_red != sim->wb_pending_red || sim->wb_blue != sim->wb_pending_blue || sim->wb_first_frame == 0U) {
                sim->wb_first_frame = frame_id;
            }
            sim->wb_red = sim->wb_pending_red;
            sim->wb_blue = sim->wb_pending_blue;
        }
        sim->in_valid &= (uint8_t)~sim_mask(in_slot);
        sim->in_head = (uint8_t)((in_slot + 1U) % handle->caps.input_buffers);
        sim->events |= NEON_ISP_EVENT_IDMA_DONE;

        sim->out_free &= (uint8_t)~sim_mask(out_slot);
        sim->out_done |= sim_mask(out_slot);
        sim->out_head = (uint8_t)((out_slot + 1U) % handle->caps.output_buffers);
        sim->out_tag[out_slot] = sim->in_tag[in_slot];
        sim->out_ts[out_slot] = sim->in_ts[in_slot];
        sim->clock += SIM_SOF_CLOCKS;
        sim->out_sof[out_slot] = sim->clock;
        sim->clock += SIM_FRAME_CLOCKS;
        sim->out_eof[out_slot] = sim->clock;
        sim->frames_out++;
        sim->events |= NEON_ISP_EVENT_ODMA_DONE | NEON_ISP_EVENT_FRAME_DONE;
        for (block = 0U; block < 3U; block++) {
            if (sim->stats_en[block] && (sim->stats_hold & (1U << block)) == 0U) {
                sim_publish(sim, (uint8_t)block, frame_id);
                sim->events |= (block == SIM_AEC) ? NEON_ISP_EVENT_STATS_AEC :
                               (block == SIM_AWB) ? NEON_ISP_EVENT_STATS_AWB : NEON_ISP_EVENT_STATS_AF;
            }
        }
        done++;
    }
    return done;
}

void fx1_isp_sim_inject_axi_error(neon_isp_handle_t *handle, bool write)
{
    if (handle != NULL) {
        isp_sim(handle)->axi_error_pending = true;
        isp_sim(handle)->axi_error_write = write;
    }
}

void fx1_isp_sim_raise_events(neon_isp_handle_t *handle, uint32_t events)
{
    if (handle != NULL) {
        isp_sim(handle)->events |= events;
    }
}

void fx1_isp_sim_set_start_fails(neon_isp_handle_t *handle, bool fails)
{
    if (handle != NULL) {
        isp_sim(handle)->start_fails = fails;
    }
}

void fx1_isp_sim_set_stop_fails(neon_isp_handle_t *handle, bool fails)
{
    if (handle != NULL) {
        isp_sim(handle)->stop_fails = fails;
    }
}

void fx1_isp_sim_set_soft_reset_fails(neon_isp_handle_t *handle, bool fails)
{
    if (handle != NULL) {
        isp_sim(handle)->soft_reset_fails = fails;
    }
}

void fx1_isp_sim_set_observer(neon_isp_handle_t *handle, fx1_isp_sim_observer_t observer, void *context)
{
    if (handle != NULL) {
        isp_sim(handle)->observer = observer;
        isp_sim(handle)->observer_context = context;
    }
}

void fx1_isp_sim_get_counters(const neon_isp_handle_t *handle, fx1_isp_sim_counters_t *counters)
{
    if (handle != NULL && counters != NULL) {
        *counters = isp_sim_const(handle)->counters;
    }
}

void fx1_isp_sim_get_applied_controls(const neon_isp_handle_t *handle, uint16_t *red_gain_q8,
                                      uint16_t *blue_gain_q8, uint32_t *first_frame_id)
{
    const isp_sim_t *sim = isp_sim_const(handle);

    *red_gain_q8 = sim->wb_red;
    *blue_gain_q8 = sim->wb_blue;
    *first_frame_id = sim->wb_first_frame;
}

uint32_t fx1_isp_sim_get_context_id(const neon_isp_handle_t *handle)
{
    return isp_sim_const(handle)->context_id;
}

uint32_t fx1_isp_sim_get_event_mask(const neon_isp_handle_t *handle)
{
    return isp_sim_const(handle)->event_mask;
}
