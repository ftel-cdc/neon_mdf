/**
 * @file test_neon_isp.c
 * @brief Host tests of the ISP HAL on the in-RAM backend.
 */

#include "test_common.h"
#include "isp/neon_isp.h"
#include "isp/platform/fx1/fx1_isp_simulation.h"
#include "fx1_registers.h"

#define WIDTH 64U
#define HEIGHT 32U
#define IN_STRIDE 128U
#define IN_BYTES (IN_STRIDE * HEIGHT)
#define Y_STRIDE 64U
#define UV_STRIDE 64U
#define Y_BYTES (Y_STRIDE * HEIGHT)
#define UV_BYTES (UV_STRIDE * (HEIGHT / 2U))
#define IN_BASE 0x80000000UL
#define Y_BASE 0x80100000UL
#define UV_BASE 0x80200000UL
#define SLOT_GAP 0x2000UL

static void make_config(neon_isp_config_t *config)
{
    uint32_t i;

    neon_isp_get_default_config(config);
    config->width = WIDTH;
    config->height = HEIGHT;
    config->input_stride_bytes = IN_STRIDE;
    config->y_stride_bytes = Y_STRIDE;
    config->uv_stride_bytes = UV_STRIDE;
    config->output_y_bytes = Y_BYTES;
    config->output_uv_bytes = UV_BYTES;
    for (i = 0U; i < NEON_ISP_MAX_BUFFERS; i++) {
        config->output_y_addr[i] = (uint32_t)(Y_BASE + i * SLOT_GAP);
        config->output_uv_addr[i] = (uint32_t)(UV_BASE + i * SLOT_GAP);
    }
}

static neon_isp_input_t make_input(uint32_t n)
{
    neon_isp_input_t input;

    (void)memset(&input, 0, sizeof(input));
    input.address = (uint32_t)(IN_BASE + n * SLOT_GAP);
    input.size_bytes = IN_BYTES;
    input.tag = 100U + n;
    input.timestamp = 5000U + n;
    return input;
}

static void configured(neon_isp_handle_t *isp)
{
    neon_isp_config_t config;

    CHECK_EQ(neon_isp_init(isp), NEON_OK);
    make_config(&config);
    CHECK_EQ(neon_isp_configure(isp, &config), NEON_OK);
}

static void running(neon_isp_handle_t *isp)
{
    configured(isp);
    CHECK_EQ(neon_isp_start(isp), NEON_OK);
}

static void queue(neon_isp_handle_t *isp, uint32_t first, uint32_t count)
{
    uint32_t i;

    for (i = 0U; i < count; i++) {
        neon_isp_input_t input = make_input(first + i);

        CHECK_EQ(neon_isp_queue_input(isp, &input), NEON_OK);
    }
}

static void test_fx1_caps(void)
{
    neon_isp_caps_t caps;

    CHECK_EQ(neon_isp_get_caps(NULL), NEON_INVALID_PARAM);
    CHECK_EQ(neon_isp_get_caps(&caps), NEON_OK);
    CHECK_EQ(caps.input_buffers, 4U);
    CHECK_EQ(caps.output_buffers, 4U);
    CHECK_EQ(caps.aec_hist_bins, 64U);
    CHECK(caps.aec && caps.awb && caps.af);
    CHECK_EQ(caps.raw_depth_mask, (1U << 10) | (1U << 12));
    CHECK_EQ(caps.bayer_pattern_mask, 0x0FU);
    CHECK_EQ(caps.min_width, 32U);
    CHECK_EQ(caps.max_width, 3842U);
    CHECK_EQ(caps.min_height, 32U);
    CHECK_EQ(caps.max_height, 2162U);
    CHECK_EQ(caps.size_granularity, 2U);
    CHECK_EQ(caps.max_burst_beats, 256U);   /* Follows the 8-bit MAX_BURST_M1 field of the register map. */
    CHECK_EQ(caps.dma_align_bytes, 16U);
    CHECK_EQ(caps.max_wb_gain_q8, 0xFFFU);  /* Follows the 12-bit gain field of the register map. */
    CHECK_EQ(caps.supported_events, 0xFFFFUL);
}

static void test_arguments_and_defaults(void)
{
    neon_isp_handle_t isp;
    neon_isp_config_t config;
    neon_isp_input_t input = make_input(0U);
    neon_isp_output_t output;
    neon_isp_statistics_t statistics;
    neon_isp_dynamic_controls_t controls = {0U, 256U, 256U};
    neon_isp_status_t status;
    uint32_t events = 1U;

    CHECK_EQ(neon_isp_init(NULL), NEON_INVALID_PARAM);
    CHECK_EQ(neon_isp_init(&isp), NEON_OK);
    CHECK_EQ(isp.state, NEON_ISP_STATE_IDLE);
    neon_isp_get_default_config(NULL); /* Must not crash. */
    neon_isp_get_default_config(&config);
    CHECK_EQ(config.raw_depth, NEON_ISP_RAW12);
    CHECK_EQ(config.bayer_pattern, NEON_ISP_BAYER_RGGB);
    CHECK_EQ(config.max_burst_beats, 16U);
    CHECK(config.stats_aec && config.stats_awb && config.stats_af);
    CHECK(config.event_mask & NEON_ISP_EVENT_ODMA_DONE);
    CHECK_EQ(config.width, 0U);

    CHECK_EQ(neon_isp_configure(NULL, &config), NEON_INVALID_PARAM);
    CHECK_EQ(neon_isp_configure(&isp, NULL), NEON_INVALID_PARAM);
    CHECK_EQ(neon_isp_start(NULL), NEON_INVALID_PARAM);
    CHECK_EQ(neon_isp_start(&isp), NEON_ERROR);            /* Not configured. */
    CHECK_EQ(neon_isp_stop(&isp), NEON_ERROR);
    CHECK_EQ(neon_isp_recover(&isp, NEON_ISP_RECOVER_SOFT), NEON_ERROR);
    CHECK_EQ(neon_isp_recover(&isp, (neon_isp_recover_level_t)7), NEON_INVALID_PARAM);
    CHECK_EQ(neon_isp_queue_input(&isp, &input), NEON_ERROR);
    CHECK_EQ(neon_isp_queue_input(&isp, NULL), NEON_INVALID_PARAM);
    CHECK_EQ(neon_isp_reclaim_input(&isp, &input), NEON_ERROR);
    CHECK_EQ(neon_isp_acquire_output(&isp, &output), NEON_ERROR);
    CHECK_EQ(neon_isp_acquire_output(&isp, NULL), NEON_INVALID_PARAM);
    CHECK_EQ(neon_isp_release_output(&isp, 0U), NEON_ERROR);
    CHECK_EQ(neon_isp_read_statistics(&isp, &statistics), NEON_ERROR);
    CHECK_EQ(neon_isp_set_context_id(&isp, 1U), NEON_ERROR);
    CHECK_EQ(neon_isp_apply_dynamic_controls(&isp, &controls, NULL), NEON_ERROR);
    CHECK_EQ(neon_isp_apply_dynamic_controls(&isp, NULL, NULL), NEON_INVALID_PARAM);
    CHECK_EQ(neon_isp_set_event_mask(&isp, 0U), NEON_ERROR);
    CHECK_EQ(neon_isp_get_events(&isp, &events), NEON_OK); /* Nothing is pending before configuration. */
    CHECK_EQ(events, 0U);
    CHECK_EQ(neon_isp_clear_events(&isp, NEON_ISP_EVENT_FRAME_DONE), NEON_OK);
    CHECK_EQ(neon_isp_get_status(&isp, &status), NEON_OK);
    CHECK_EQ(status.state, NEON_ISP_STATE_IDLE);
    CHECK_EQ(neon_isp_get_status(&isp, NULL), NEON_INVALID_PARAM);
    CHECK_EQ(neon_isp_deinit(NULL), NEON_INVALID_PARAM);
    CHECK_EQ(neon_isp_deinit(&isp), NEON_OK);
}

static void test_configure_validation(void)
{
    neon_isp_handle_t isp;
    neon_isp_config_t good;
    neon_isp_config_t config;
    uint8_t i;

    CHECK_EQ(neon_isp_init(&isp), NEON_OK);
    make_config(&good);

    config = good;
    config.width = 30U; /* Below the minimum. */
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_INVALID_PARAM);
    config = good;
    config.width = 3844U;
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_INVALID_PARAM);
    config = good;
    config.width = 65U; /* Odd. */
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_INVALID_PARAM);
    config = good;
    config.height = 33U;
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_INVALID_PARAM);
    config = good;
    config.height = 2164U;
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_INVALID_PARAM);
    config = good;
    config.raw_depth = (neon_isp_raw_depth_t)8;
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_INVALID_PARAM);
    config = good;
    config.raw_depth = NEON_ISP_RAW10;
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_OK);
    config = good;
    config.bayer_pattern = (neon_isp_bayer_pattern_t)4;
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_INVALID_PARAM);
    config.bayer_pattern = NEON_ISP_BAYER_BGGR;
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_OK);
    config = good;
    config.max_burst_beats = 0U;
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_INVALID_PARAM);
    config.max_burst_beats = 257U;
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_INVALID_PARAM);
    config.max_burst_beats = 256U;
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_OK);
    config = good;
    config.event_mask = 0x10000UL; /* A bit that the chip cannot raise. */
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_INVALID_PARAM);

    /* Strides: at least the line, a multiple of the AXI beat. */
    config = good;
    config.input_stride_bytes = 2U * WIDTH - 16U;
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_INVALID_PARAM);
    config = good;
    config.input_stride_bytes = 2U * WIDTH + 8U; /* Not a multiple of 16. */
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_INVALID_PARAM);
    config = good;
    config.y_stride_bytes = WIDTH - 16U;
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_INVALID_PARAM);
    config = good;
    config.uv_stride_bytes = WIDTH + 4U;
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_INVALID_PARAM);
    config = good;
    config.y_stride_bytes = 96U; /* A larger stride is fine when the buffer is large enough. */
    config.output_y_bytes = 96U * HEIGHT;
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_OK);
    config.output_y_bytes = 96U * HEIGHT - 1U;
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_INVALID_PARAM);

    /* Output buffers: size, alignment, wrap, overlap. */
    config = good;
    config.output_uv_bytes = UV_BYTES - 1U;
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_INVALID_PARAM);
    for (i = 0U; i < 4U; i++) {
        config = good;
        config.output_y_addr[i] = 0U;
        CHECK_EQ(neon_isp_configure(&isp, &config), NEON_INVALID_PARAM);
        config = good;
        config.output_uv_addr[i] += 4U; /* Not aligned. */
        CHECK_EQ(neon_isp_configure(&isp, &config), NEON_INVALID_PARAM);
    }
    config = good;
    config.output_uv_addr[2] = 0xFFFFFF00UL; /* Runs past the end of the 32-bit space. */
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_INVALID_PARAM);
    config = good;
    config.output_y_addr[1] = config.output_y_addr[0] + Y_BYTES - 16U; /* Two luma buffers overlap. */
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_INVALID_PARAM);
    config = good;
    config.output_uv_addr[3] = config.output_y_addr[3] + 16U; /* A luma and a chroma buffer overlap. */
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_INVALID_PARAM);
    config = good;
    config.output_uv_addr[0] = config.output_y_addr[1]; /* Chroma of one slot on luma of another. */
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_INVALID_PARAM);
    config = good;
    config.output_y_addr[7] = 0U; /* Slots beyond caps.output_buffers are ignored. */
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_OK);
}

static void test_core_follows_caps(void)
{
    neon_isp_handle_t isp;
    neon_isp_config_t config;
    neon_isp_input_t input;
    neon_isp_input_t back;
    uint32_t i;

    /* Another chip: two buffers each way, no focus block, RAW12 only, 64-byte AXI port, tighter sizes. */
    CHECK_EQ(neon_isp_init(&isp), NEON_OK);
    isp.caps.input_buffers = 2U;
    isp.caps.output_buffers = 2U;
    isp.caps.af = false;
    isp.caps.raw_depth_mask = (uint16_t)(1U << 12);
    isp.caps.bayer_pattern_mask = 0x01U;
    isp.caps.min_width = 64U;
    isp.caps.max_width = 128U;
    isp.caps.size_granularity = 4U;
    isp.caps.max_burst_beats = 16U;
    isp.caps.dma_align_bytes = 64U;
    isp.caps.max_wb_gain_q8 = 0x3FFU;
    isp.caps.supported_events &= ~(uint32_t)NEON_ISP_EVENT_STATS_AF;

    make_config(&config);
    config.stats_af = true;
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_NOT_SUPPORTED);
    config.stats_af = false;
    config.event_mask &= ~(uint32_t)NEON_ISP_EVENT_STATS_AF;
    config.max_burst_beats = 16U;
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_OK);
    config.output_y_addr[0] += 16U; /* A multiple of 16 but not of this chip's 64. */
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_INVALID_PARAM);
    config.output_y_addr[0] -= 16U;
    for (i = 0U; i < 2U; i++) {
        config.output_y_addr[i] = (uint32_t)(Y_BASE + i * SLOT_GAP);
        config.output_uv_addr[i] = (uint32_t)(UV_BASE + i * SLOT_GAP);
    }
    config.raw_depth = NEON_ISP_RAW10;
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_NOT_SUPPORTED);
    config.raw_depth = NEON_ISP_RAW12;
    config.bayer_pattern = NEON_ISP_BAYER_GRBG;
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_NOT_SUPPORTED);
    config.bayer_pattern = NEON_ISP_BAYER_RGGB;
    config.width = 62U; /* Below this chip's minimum, and not a multiple of 4. */
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_INVALID_PARAM);
    config.width = 66U;
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_INVALID_PARAM);
    config.width = 64U;
    config.max_burst_beats = 32U;
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_INVALID_PARAM);
    config.max_burst_beats = 16U;
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_OK);

    /* The input rotation has two slots, and the input address must be 64-byte aligned. */
    input = make_input(0U);
    input.address += 16U;
    CHECK_EQ(neon_isp_queue_input(&isp, &input), NEON_INVALID_PARAM);
    queue(&isp, 0U, 2U);
    input = make_input(2U);
    CHECK_EQ(neon_isp_queue_input(&isp, &input), NEON_BUSY);
    CHECK_EQ(neon_isp_start(&isp), NEON_OK);
    CHECK_EQ(fx1_isp_sim_process(&isp, 8U), 2U);
    CHECK_EQ(neon_isp_reclaim_input(&isp, &back), NEON_OK);
    CHECK_EQ(back.tag, 100U);
    CHECK_EQ(neon_isp_reclaim_input(&isp, &back), NEON_OK);
    CHECK_EQ(back.tag, 101U);

    /* The gain limit of this chip. */
    {
        neon_isp_dynamic_controls_t controls = {0U, 0x3FFU, 256U};

        CHECK_EQ(neon_isp_apply_dynamic_controls(&isp, &controls, NULL), NEON_OK);
        controls.wb_red_gain_q8 = 0x400U;
        CHECK_EQ(neon_isp_apply_dynamic_controls(&isp, &controls, NULL), NEON_INVALID_PARAM);
    }
}

static void test_states_and_start_stop(void)
{
    neon_isp_handle_t isp;
    neon_isp_config_t config;
    neon_isp_status_t status;
    neon_isp_output_t output;

    configured(&isp);
    CHECK_EQ(isp.state, NEON_ISP_STATE_CONFIGURED);
    CHECK_EQ(neon_isp_stop(&isp), NEON_ERROR);
    CHECK_EQ(neon_isp_acquire_output(&isp, &output), NEON_ERROR); /* Outputs exist only while running. */
    CHECK_EQ(neon_isp_start(&isp), NEON_OK);
    CHECK_EQ(isp.state, NEON_ISP_STATE_RUNNING);
    CHECK_EQ(neon_isp_start(&isp), NEON_BUSY);
    make_config(&config);
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_BUSY);
    CHECK_EQ(neon_isp_deinit(&isp), NEON_BUSY);
    CHECK_EQ(neon_isp_get_status(&isp, &status), NEON_OK);
    CHECK_EQ(status.state, NEON_ISP_STATE_RUNNING);
    CHECK_EQ(neon_isp_stop(&isp), NEON_OK);
    CHECK_EQ(isp.state, NEON_ISP_STATE_CONFIGURED);
    CHECK_EQ(neon_isp_start(&isp), NEON_OK); /* Restart on the same configuration. */
    CHECK_EQ(neon_isp_stop(&isp), NEON_OK);
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_OK);
    CHECK_EQ(neon_isp_deinit(&isp), NEON_OK);
}

typedef struct {
    fx1_isp_sim_op_t op[8];
    unsigned count;
} order_t;

static void observe(void *context, fx1_isp_sim_op_t operation)
{
    order_t *order = context;

    if (order->count < 8U) {
        order->op[order->count++] = operation;
    }
}

static void test_operation_order(void)
{
    order_t order;
    neon_isp_handle_t isp;
    neon_isp_config_t config;
    fx1_isp_sim_counters_t counters;

    (void)memset(&order, 0, sizeof(order));
    CHECK_EQ(neon_isp_init(&isp), NEON_OK);
    fx1_isp_sim_set_observer(&isp, observe, &order);
    make_config(&config);
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_OK);
    CHECK_EQ(neon_isp_start(&isp), NEON_OK);
    CHECK_EQ(neon_isp_recover(&isp, NEON_ISP_RECOVER_SOFT), NEON_OK);
    CHECK_EQ(neon_isp_stop(&isp), NEON_OK);
    CHECK_EQ(order.count, 4U);
    CHECK_EQ(order.op[0], FX1_ISP_SIM_OP_CONFIGURE);
    CHECK_EQ(order.op[1], FX1_ISP_SIM_OP_START);
    CHECK_EQ(order.op[2], FX1_ISP_SIM_OP_SOFT_RESET); /* A recover is a reset and a re-arm, not a stop and start. */
    CHECK_EQ(order.op[3], FX1_ISP_SIM_OP_STOP);
    fx1_isp_sim_get_counters(&isp, &counters);
    CHECK_EQ(counters.configures, 1U);
    CHECK_EQ(counters.starts, 1U);
    CHECK_EQ(counters.soft_resets, 1U);
    CHECK_EQ(counters.stops, 1U);
}

static void test_frame_flow(void)
{
    neon_isp_handle_t isp;
    neon_isp_output_t output;
    neon_isp_input_t back;
    neon_isp_status_t status;
    uint32_t i;

    running(&isp);
    CHECK_EQ(neon_isp_acquire_output(&isp, &output), NEON_BUSY);
    queue(&isp, 0U, 3U);
    CHECK_EQ(neon_isp_get_status(&isp, &status), NEON_OK);
    CHECK_EQ(status.input_queued_mask, 0x07U);
    CHECK_EQ(neon_isp_reclaim_input(&isp, &back), NEON_BUSY); /* The DMA has not read it yet. */
    CHECK_EQ(fx1_isp_sim_process(&isp, 2U), 2U);
    CHECK_EQ(neon_isp_get_status(&isp, &status), NEON_OK);
    CHECK_EQ(status.input_queued_mask, 0x04U);
    CHECK_EQ(status.output_done_mask, 0x03U);
    CHECK_EQ(status.frames_in, 2U);
    CHECK_EQ(status.frames_out, 2U);

    /* Oldest first, with the tag and timestamp of the input it was made from. */
    CHECK_EQ(neon_isp_acquire_output(&isp, &output), NEON_OK);
    CHECK_EQ(output.buffer_index, 0U);
    CHECK_EQ(output.y_addr, Y_BASE);
    CHECK_EQ(output.uv_addr, UV_BASE);
    CHECK_EQ(output.y_stride_bytes, Y_STRIDE);
    CHECK_EQ(output.uv_stride_bytes, UV_STRIDE);
    CHECK_EQ(output.width, WIDTH);
    CHECK_EQ(output.height, HEIGHT);
    CHECK_EQ(output.tag, 100U);
    CHECK_EQ(output.timestamp, 5000U);
    CHECK(output.eof_count - output.sof_count > 0U);
    CHECK_EQ(neon_isp_acquire_output(&isp, &output), NEON_OK);
    CHECK_EQ(output.buffer_index, 1U);
    CHECK_EQ(output.y_addr, Y_BASE + SLOT_GAP);
    CHECK_EQ(output.tag, 101U);
    CHECK_EQ(neon_isp_acquire_output(&isp, &output), NEON_BUSY);
    CHECK_EQ(neon_isp_get_status(&isp, &status), NEON_OK);
    CHECK_EQ(status.output_held_mask, 0x03U);
    CHECK_EQ(status.output_done_mask, 0x00U);

    /* The consumed inputs come back in the order they went in. */
    CHECK_EQ(neon_isp_reclaim_input(&isp, &back), NEON_OK);
    CHECK_EQ(back.tag, 100U);
    CHECK_EQ(back.address, IN_BASE);
    CHECK(back.processed);
    CHECK_EQ(neon_isp_reclaim_input(&isp, &back), NEON_OK);
    CHECK_EQ(back.tag, 101U);
    CHECK_EQ(neon_isp_reclaim_input(&isp, &back), NEON_BUSY); /* The third is still queued. */

    /* Release, then the rotation continues past the wrap, in order. */
    CHECK_EQ(neon_isp_release_output(&isp, 0U), NEON_OK);
    CHECK_EQ(neon_isp_release_output(&isp, 0U), NEON_INVALID_PARAM); /* No longer held. */
    CHECK_EQ(neon_isp_release_output(&isp, 1U), NEON_OK);
    for (i = 3U; i < 12U; i++) {
        neon_isp_input_t input = make_input(i);

        CHECK_EQ(neon_isp_queue_input(&isp, &input), NEON_OK);
        CHECK_EQ(fx1_isp_sim_process(&isp, 1U), 1U);
        CHECK_EQ(neon_isp_reclaim_input(&isp, &back), NEON_OK);
        CHECK_EQ(neon_isp_acquire_output(&isp, &output), NEON_OK);
        CHECK_EQ(neon_isp_release_output(&isp, output.buffer_index), NEON_OK);
    }
    CHECK_EQ(neon_isp_get_status(&isp, &status), NEON_OK);
    CHECK_EQ(status.frames_out, 11U);
}

static void test_output_order_across_wrap(void)
{
    neon_isp_handle_t isp;
    neon_isp_output_t output;
    neon_isp_input_t back;
    uint32_t n;
    uint32_t tag = 100U;

    running(&isp);
    for (n = 0U; n < 10U; n++) { /* Ten frames through four output slots. */
        neon_isp_input_t input = make_input(n);

        CHECK_EQ(neon_isp_queue_input(&isp, &input), NEON_OK);
        CHECK_EQ(fx1_isp_sim_process(&isp, 1U), 1U);
        CHECK_EQ(neon_isp_reclaim_input(&isp, &back), NEON_OK);
        CHECK_EQ(neon_isp_acquire_output(&isp, &output), NEON_OK);
        CHECK_EQ(output.tag, tag);
        CHECK_EQ(output.buffer_index, n % 4U);
        tag++;
        CHECK_EQ(neon_isp_release_output(&isp, output.buffer_index), NEON_OK);
    }
}

static void test_input_queue_rules(void)
{
    neon_isp_handle_t isp;
    neon_isp_input_t input;
    neon_isp_input_t back;
    neon_isp_output_t output;

    configured(&isp);
    input = make_input(0U);
    input.address = 0U;
    CHECK_EQ(neon_isp_queue_input(&isp, &input), NEON_INVALID_PARAM);
    input = make_input(0U);
    input.address += 8U; /* Not aligned. */
    CHECK_EQ(neon_isp_queue_input(&isp, &input), NEON_INVALID_PARAM);
    input = make_input(0U);
    input.size_bytes = IN_BYTES - 1U; /* Smaller than stride * height. */
    CHECK_EQ(neon_isp_queue_input(&isp, &input), NEON_INVALID_PARAM);
    input = make_input(0U);
    input.address = 0xFFFFF800UL; /* Runs past the end of the 32-bit space. */
    CHECK_EQ(neon_isp_queue_input(&isp, &input), NEON_INVALID_PARAM);

    /* Frames may be queued before the start; the rotation holds four, and the fifth waits. */
    queue(&isp, 0U, 4U);
    input = make_input(4U);
    CHECK_EQ(neon_isp_queue_input(&isp, &input), NEON_BUSY);
    CHECK_EQ(neon_isp_start(&isp), NEON_OK);
    CHECK_EQ(fx1_isp_sim_process(&isp, 1U), 1U);
    CHECK_EQ(neon_isp_reclaim_input(&isp, &back), NEON_OK);
    CHECK_EQ(back.tag, 100U);
    CHECK_EQ(neon_isp_acquire_output(&isp, &output), NEON_OK);
    CHECK_EQ(neon_isp_release_output(&isp, output.buffer_index), NEON_OK);
    CHECK_EQ(neon_isp_queue_input(&isp, &input), NEON_OK); /* A slot is free again. */
    CHECK_EQ(fx1_isp_sim_process(&isp, 8U), 4U);
    CHECK_EQ(neon_isp_reclaim_input(&isp, &back), NEON_OK);
    CHECK_EQ(back.tag, 101U);
}

static void test_output_starvation_and_underrun(void)
{
    neon_isp_handle_t isp;
    neon_isp_output_t held[4];
    neon_isp_output_t output;
    uint32_t events = 0U;
    uint32_t i;

    running(&isp);
    /* No input: the input DMA reports an underrun and nothing moves. */
    CHECK_EQ(fx1_isp_sim_process(&isp, 4U), 0U);
    CHECK_EQ(neon_isp_get_events(&isp, &events), NEON_OK);
    CHECK(events & NEON_ISP_EVENT_IDMA_UNDERRUN);
    CHECK_EQ(neon_isp_clear_events(&isp, NEON_ISP_EVENT_IDMA_UNDERRUN), NEON_OK);

    /* Software holds every output: the fifth frame waits, loses nothing, and the overflow is reported. */
    queue(&isp, 0U, 4U);
    CHECK_EQ(fx1_isp_sim_process(&isp, 8U), 4U);
    for (i = 0U; i < 4U; i++) {
        CHECK_EQ(neon_isp_acquire_output(&isp, &held[i]), NEON_OK);
    }
    {
        neon_isp_input_t back;
        neon_isp_input_t input = make_input(4U);

        for (i = 0U; i < 4U; i++) {
            CHECK_EQ(neon_isp_reclaim_input(&isp, &back), NEON_OK);
        }
        CHECK_EQ(neon_isp_queue_input(&isp, &input), NEON_OK);
    }
    CHECK_EQ(fx1_isp_sim_process(&isp, 2U), 0U);
    CHECK_EQ(neon_isp_get_events(&isp, &events), NEON_OK);
    CHECK(events & NEON_ISP_EVENT_ODMA_OVERFLOW);
    CHECK_EQ(neon_isp_release_output(&isp, held[0].buffer_index), NEON_OK);
    CHECK_EQ(fx1_isp_sim_process(&isp, 2U), 1U); /* Resumes with the frame that waited. */
    CHECK_EQ(neon_isp_acquire_output(&isp, &output), NEON_OK);
    CHECK_EQ(output.tag, 104U);
    CHECK_EQ(output.buffer_index, held[0].buffer_index);
}

static void test_statistics(void)
{
    neon_isp_handle_t isp;
    neon_isp_statistics_t statistics;
    neon_isp_input_t back;
    fx1_isp_sim_scene_t scene = {96U, 88U, 100U, 112U, 1234U, 5U, 7U};

    running(&isp);
    CHECK_EQ(neon_isp_read_statistics(&isp, &statistics), NEON_BUSY); /* No frame has gone through. */
    fx1_isp_sim_set_scene(&isp, &scene);
    CHECK_EQ(neon_isp_set_context_id(&isp, 77U), NEON_OK);
    queue(&isp, 0U, 2U);
    CHECK_EQ(fx1_isp_sim_process(&isp, 1U), 1U);
    CHECK_EQ(neon_isp_set_context_id(&isp, 78U), NEON_OK); /* Takes effect from the next frame. */

    CHECK_EQ(neon_isp_read_statistics(&isp, &statistics), NEON_OK);
    CHECK_EQ(statistics.frame_id, 1U);
    CHECK_EQ(statistics.context_id, 77U);
    CHECK_EQ(statistics.mean_luma, 96U);
    CHECK_EQ(statistics.mean_red, 88U);
    CHECK_EQ(statistics.mean_green, 100U);
    CHECK_EQ(statistics.mean_blue, 112U);
    CHECK_EQ(statistics.focus_metric, 1234U);
    CHECK(statistics.aec.valid && statistics.awb.valid && statistics.af.valid);
    CHECK_EQ(statistics.aec.count, WIDTH * HEIGHT);
    CHECK_EQ(statistics.aec.sum, (96ULL << 4) * WIDTH * HEIGHT);
    CHECK_EQ(statistics.aec.under_count, 5U);
    CHECK_EQ(statistics.aec.over_count, 7U);
    CHECK(statistics.aec.histogram_valid);
    CHECK_EQ(statistics.aec.histogram[(96U << 4) >> 6], WIDTH * HEIGHT);
    CHECK_EQ(statistics.aec.histogram[0], 0U);
    CHECK_EQ(statistics.awb.count, WIDTH * HEIGHT / 4U);
    CHECK_EQ(statistics.awb.sum_r, (88ULL << 4) * (WIDTH * HEIGHT / 4U));
    CHECK_EQ(statistics.awb.frame_id, 1U);
    CHECK_EQ(statistics.af.focus, 1234U);
    CHECK_EQ(neon_isp_read_statistics(&isp, &statistics), NEON_BUSY); /* Nothing newer. */

    CHECK_EQ(fx1_isp_sim_process(&isp, 1U), 1U);
    CHECK_EQ(neon_isp_read_statistics(&isp, &statistics), NEON_OK);
    CHECK_EQ(statistics.frame_id, 2U);
    CHECK_EQ(statistics.context_id, 78U);
    CHECK_EQ(neon_isp_reclaim_input(&isp, &back), NEON_OK);

    /* A focus value beyond 16 bits saturates the digest and keeps the full value. */
    scene.focus = 70000U;
    fx1_isp_sim_set_scene(&isp, &scene);
    queue(&isp, 2U, 1U);
    CHECK_EQ(fx1_isp_sim_process(&isp, 1U), 1U);
    CHECK_EQ(neon_isp_read_statistics(&isp, &statistics), NEON_OK);
    CHECK_EQ(statistics.focus_metric, 0xFFFFU);
    CHECK_EQ(statistics.af.focus, 70000U);
}

static void test_statistics_generators_off(void)
{
    neon_isp_handle_t isp;
    neon_isp_config_t config;
    neon_isp_statistics_t statistics;

    CHECK_EQ(neon_isp_init(&isp), NEON_OK);
    make_config(&config);
    config.stats_aec = false;
    config.stats_awb = false;
    config.stats_af = true;
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_OK);
    CHECK_EQ(neon_isp_start(&isp), NEON_OK);
    queue(&isp, 0U, 1U);
    CHECK_EQ(fx1_isp_sim_process(&isp, 1U), 1U);
    CHECK_EQ(neon_isp_read_statistics(&isp, &statistics), NEON_OK);
    CHECK(!statistics.aec.valid);
    CHECK(!statistics.awb.valid);
    CHECK(statistics.af.valid);
    CHECK_EQ(statistics.frame_id, 1U);     /* Taken from the first generator that has a result. */
    CHECK_EQ(statistics.mean_luma, 0U);    /* A generator without a result gives zero, which a 3A refuses. */
    CHECK_EQ(statistics.mean_red, 0U);

    CHECK_EQ(neon_isp_stop(&isp), NEON_OK);
    make_config(&config);
    config.stats_aec = false;
    config.stats_awb = false;
    config.stats_af = false;
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_OK);
    CHECK_EQ(neon_isp_start(&isp), NEON_OK);
    queue(&isp, 0U, 1U);
    CHECK_EQ(fx1_isp_sim_process(&isp, 1U), 1U);
    CHECK_EQ(neon_isp_read_statistics(&isp, &statistics), NEON_BUSY); /* Nothing is measured. */
}

static void test_dynamic_controls(void)
{
    neon_isp_handle_t isp;
    neon_isp_dynamic_controls_t controls = {9U, 300U, 400U};
    uint32_t effective = 0U;
    uint16_t red;
    uint16_t blue;
    uint32_t first;

    running(&isp);
    queue(&isp, 0U, 2U);
    CHECK_EQ(fx1_isp_sim_process(&isp, 2U), 2U);
    controls.wb_red_gain_q8 = 0U; /* Zero would black out a channel. */
    CHECK_EQ(neon_isp_apply_dynamic_controls(&isp, &controls, &effective), NEON_INVALID_PARAM);
    controls.wb_red_gain_q8 = 300U;
    controls.wb_blue_gain_q8 = 0U;
    CHECK_EQ(neon_isp_apply_dynamic_controls(&isp, &controls, &effective), NEON_INVALID_PARAM);
    controls.wb_blue_gain_q8 = 0x1000U; /* Beyond the 12-bit gain field. */
    CHECK_EQ(neon_isp_apply_dynamic_controls(&isp, &controls, &effective), NEON_INVALID_PARAM);
    controls.wb_blue_gain_q8 = 0xFFFU;
    CHECK_EQ(neon_isp_apply_dynamic_controls(&isp, &controls, &effective), NEON_OK);
    CHECK_EQ(effective, 3U); /* Two frames have started; the next one is the third. */
    controls.wb_blue_gain_q8 = 400U;
    CHECK_EQ(neon_isp_apply_dynamic_controls(&isp, &controls, NULL), NEON_OK);

    fx1_isp_sim_get_applied_controls(&isp, &red, &blue, &first);
    CHECK_EQ(first, 0U); /* No frame has started since the write. */
    queue(&isp, 2U, 1U);
    CHECK_EQ(fx1_isp_sim_process(&isp, 1U), 1U);
    fx1_isp_sim_get_applied_controls(&isp, &red, &blue, &first);
    CHECK_EQ(red, 300U);
    CHECK_EQ(blue, 400U);
    CHECK_EQ(first, effective);
}

static void test_events(void)
{
    neon_isp_handle_t isp;
    neon_isp_status_t status;
    uint32_t events = 0U;

    running(&isp);
    CHECK_EQ(neon_isp_set_event_mask(&isp, 0x10000UL), NEON_INVALID_PARAM);
    CHECK_EQ(neon_isp_set_event_mask(&isp, NEON_ISP_EVENT_ODMA_DONE), NEON_OK);
    CHECK_EQ(fx1_isp_sim_get_event_mask(&isp), NEON_ISP_EVENT_ODMA_DONE);
    CHECK_EQ(neon_isp_get_status(&isp, &status), NEON_OK);
    CHECK(!status.irq_pending);
    queue(&isp, 0U, 1U);
    CHECK_EQ(fx1_isp_sim_process(&isp, 1U), 1U);
    CHECK_EQ(neon_isp_get_events(&isp, &events), NEON_OK);
    CHECK((events & (NEON_ISP_EVENT_IDMA_START | NEON_ISP_EVENT_IDMA_DONE | NEON_ISP_EVENT_ODMA_DONE |
                     NEON_ISP_EVENT_FRAME_DONE | NEON_ISP_EVENT_STATS_AEC | NEON_ISP_EVENT_STATS_AWB |
                     NEON_ISP_EVENT_STATS_AF)) ==
          (NEON_ISP_EVENT_IDMA_START | NEON_ISP_EVENT_IDMA_DONE | NEON_ISP_EVENT_ODMA_DONE |
           NEON_ISP_EVENT_FRAME_DONE | NEON_ISP_EVENT_STATS_AEC | NEON_ISP_EVENT_STATS_AWB |
           NEON_ISP_EVENT_STATS_AF));
    CHECK_EQ(neon_isp_get_status(&isp, &status), NEON_OK);
    CHECK(status.irq_pending);

    /* Clear only what was handled: the others stay, and a masked event stays visible to a poller. */
    CHECK_EQ(neon_isp_clear_events(&isp, NEON_ISP_EVENT_ODMA_DONE), NEON_OK);
    CHECK_EQ(neon_isp_get_events(&isp, &events), NEON_OK);
    CHECK(!(events & NEON_ISP_EVENT_ODMA_DONE));
    CHECK(events & NEON_ISP_EVENT_FRAME_DONE);
    CHECK_EQ(neon_isp_get_status(&isp, &status), NEON_OK);
    CHECK(!status.irq_pending); /* The only enabled event is gone. */
    CHECK_EQ(neon_isp_clear_events(&isp, 0x10000UL), NEON_OK); /* A bit the chip does not have: ignored. */

    /* A response error aborts the frame: the rotation does not advance and nothing is consumed. */
    fx1_isp_sim_inject_axi_error(&isp, false);
    queue(&isp, 1U, 1U);
    CHECK_EQ(fx1_isp_sim_process(&isp, 1U), 0U);
    CHECK_EQ(neon_isp_get_events(&isp, &events), NEON_OK);
    CHECK(events & NEON_ISP_EVENT_AXI_READ_ERROR);
    CHECK(events & NEON_ISP_EVENT_PIPELINE_ERROR);
    CHECK_EQ(fx1_isp_sim_process(&isp, 1U), 1U); /* The next attempt goes through. */
    fx1_isp_sim_inject_axi_error(&isp, true);
    queue(&isp, 2U, 1U);
    CHECK_EQ(fx1_isp_sim_process(&isp, 1U), 0U);
    CHECK_EQ(neon_isp_get_events(&isp, &events), NEON_OK);
    CHECK(events & NEON_ISP_EVENT_AXI_WRITE_ERROR);
}

static void test_stop_discards_and_flushes(void)
{
    neon_isp_handle_t isp;
    neon_isp_output_t output;
    neon_isp_input_t back;
    neon_isp_input_t input = make_input(9U);

    running(&isp);
    queue(&isp, 0U, 3U);
    CHECK_EQ(fx1_isp_sim_process(&isp, 1U), 1U);
    CHECK_EQ(neon_isp_acquire_output(&isp, &output), NEON_OK);
    CHECK_EQ(neon_isp_stop(&isp), NEON_OK);
    CHECK_EQ(neon_isp_acquire_output(&isp, &output), NEON_ERROR);
    CHECK_EQ(neon_isp_release_output(&isp, 0U), NEON_ERROR);

    /* The consumed frame is a processed one; the two that never got read are returned as thrown away. */
    CHECK_EQ(neon_isp_queue_input(&isp, &input), NEON_BUSY); /* Reclaim them first: the rotation restarts at slot 0. */
    CHECK_EQ(neon_isp_reclaim_input(&isp, &back), NEON_OK);
    CHECK_EQ(back.tag, 100U);
    CHECK(back.processed);
    CHECK_EQ(neon_isp_reclaim_input(&isp, &back), NEON_OK);
    CHECK_EQ(back.tag, 101U);
    CHECK(!back.processed);
    CHECK_EQ(neon_isp_queue_input(&isp, &input), NEON_BUSY); /* One is still to come back. */
    CHECK_EQ(neon_isp_reclaim_input(&isp, &back), NEON_OK);
    CHECK_EQ(back.tag, 102U);
    CHECK(!back.processed);
    CHECK_EQ(neon_isp_reclaim_input(&isp, &back), NEON_BUSY);

    /* Everything is back: a new run starts from the first slot and the first output. */
    CHECK_EQ(neon_isp_start(&isp), NEON_OK);
    CHECK_EQ(neon_isp_queue_input(&isp, &input), NEON_OK);
    CHECK_EQ(fx1_isp_sim_process(&isp, 1U), 1U);
    CHECK_EQ(neon_isp_acquire_output(&isp, &output), NEON_OK);
    CHECK_EQ(output.buffer_index, 0U);
    CHECK_EQ(output.tag, 109U);
}

static void test_recover(void)
{
    neon_isp_handle_t isp;
    neon_isp_output_t output;
    neon_isp_output_t held;
    neon_isp_input_t back;
    neon_isp_statistics_t statistics;
    uint32_t events = 0U;
    uint32_t i;

    running(&isp);
    queue(&isp, 0U, 4U);
    CHECK_EQ(fx1_isp_sim_process(&isp, 2U), 2U);
    CHECK_EQ(neon_isp_acquire_output(&isp, &held), NEON_OK);
    fx1_isp_sim_set_soft_reset_fails(&isp, true);
    CHECK_EQ(neon_isp_recover(&isp, NEON_ISP_RECOVER_SOFT), NEON_ERROR);
    CHECK_EQ(isp.state, NEON_ISP_STATE_RUNNING);
    CHECK_EQ(neon_isp_release_output(&isp, held.buffer_index), NEON_OK); /* A failed recover changed nothing. */
    CHECK_EQ(neon_isp_acquire_output(&isp, &held), NEON_OK);
    fx1_isp_sim_set_soft_reset_fails(&isp, false);

    CHECK_EQ(neon_isp_recover(&isp, NEON_ISP_RECOVER_SOFT), NEON_OK);
    CHECK_EQ(isp.state, NEON_ISP_STATE_RUNNING);
    CHECK_EQ(neon_isp_release_output(&isp, held.buffer_index), NEON_INVALID_PARAM); /* The frame is lost. */
    CHECK_EQ(neon_isp_get_events(&isp, &events), NEON_OK);
    CHECK_EQ(events, 0U);                                                           /* Pending events went. */
    CHECK_EQ(neon_isp_read_statistics(&isp, &statistics), NEON_BUSY);               /* So did the results. */
    CHECK_EQ(neon_isp_acquire_output(&isp, &output), NEON_BUSY);                    /* Done frames are gone. */

    /* Slot 0 and 1 were read, the two others were queued and are thrown away. */
    CHECK_EQ(neon_isp_reclaim_input(&isp, &back), NEON_OK);
    CHECK(back.processed);
    CHECK_EQ(neon_isp_reclaim_input(&isp, &back), NEON_OK);
    CHECK(back.processed);
    CHECK_EQ(neon_isp_reclaim_input(&isp, &back), NEON_OK);
    CHECK_EQ(back.tag, 102U);
    CHECK(!back.processed);
    CHECK_EQ(neon_isp_reclaim_input(&isp, &back), NEON_OK);
    CHECK_EQ(back.tag, 103U);
    CHECK(!back.processed);

    /* The pipeline is enabled, every output is free again, and the rotations start from zero. */
    for (i = 0U; i < 4U; i++) {
        neon_isp_input_t input = make_input(10U + i);

        CHECK_EQ(neon_isp_queue_input(&isp, &input), NEON_OK);
    }
    CHECK_EQ(fx1_isp_sim_process(&isp, 8U), 4U);
    for (i = 0U; i < 4U; i++) {
        CHECK_EQ(neon_isp_acquire_output(&isp, &output), NEON_OK);
        CHECK_EQ(output.buffer_index, i);
        CHECK_EQ(output.tag, 110U + i);
    }
}

static void test_failures_keep_state(void)
{
    neon_isp_handle_t isp;
    neon_isp_input_t back;

    configured(&isp);
    fx1_isp_sim_set_start_fails(&isp, true);
    CHECK_EQ(neon_isp_start(&isp), NEON_ERROR);
    CHECK_EQ(isp.state, NEON_ISP_STATE_CONFIGURED);
    fx1_isp_sim_set_start_fails(&isp, false);
    CHECK_EQ(neon_isp_start(&isp), NEON_OK);

    queue(&isp, 0U, 1U);
    fx1_isp_sim_set_stop_fails(&isp, true);
    CHECK_EQ(neon_isp_stop(&isp), NEON_ERROR);
    CHECK_EQ(isp.state, NEON_ISP_STATE_RUNNING); /* The DMA may still run: stay running. */
    CHECK_EQ(neon_isp_reclaim_input(&isp, &back), NEON_BUSY); /* The queued frame was not thrown away. */
    fx1_isp_sim_set_stop_fails(&isp, false);
    CHECK_EQ(neon_isp_stop(&isp), NEON_OK);
}

static void test_reconfigure_forgets_inputs(void)
{
    neon_isp_handle_t isp;
    neon_isp_config_t config;
    neon_isp_input_t back;

    configured(&isp);
    queue(&isp, 0U, 2U);
    make_config(&config);
    config.width = 96U;
    config.input_stride_bytes = 192U;
    config.y_stride_bytes = 96U;
    config.uv_stride_bytes = 96U;
    config.output_y_bytes = 96U * HEIGHT;
    config.output_uv_bytes = 96U * HEIGHT / 2U;
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_OK);
    CHECK_EQ(neon_isp_reclaim_input(&isp, &back), NEON_BUSY); /* The books were cleared. */
    {
        neon_isp_input_t input = make_input(0U);

        input.size_bytes = IN_BYTES; /* Too small for the new stride. */
        CHECK_EQ(neon_isp_queue_input(&isp, &input), NEON_INVALID_PARAM);
        input.size_bytes = 192U * HEIGHT;
        CHECK_EQ(neon_isp_queue_input(&isp, &input), NEON_OK);
    }
}

/* ----------------------------------------------------------------------------------------------- */
/* Tuning profiles                                                                                  */
/* ----------------------------------------------------------------------------------------------- */
static void put_entry(uint8_t *profile, uint32_t index, uint32_t offset, uint32_t value)
{
    uint32_t i;

    for (i = 0U; i < 4U; i++) {
        profile[index * 8U + i] = (uint8_t)(offset >> (8U * i));
        profile[index * 8U + 4U + i] = (uint8_t)(value >> (8U * i));
    }
}

static uint32_t fnv_word(uint32_t hash, uint32_t word)
{
    uint32_t i;

    for (i = 0U; i < 4U; i++) {
        hash ^= (word >> (8U * i)) & 0xFFU;
        hash *= 16777619U;
    }
    return hash;
}

/* A small profile in the shape of the real ones: white balance, an LSC load sequence, and two registers that
 * the HAL owns. Built one byte past an aligned address, as a file buffer may be. */
#define PROFILE_ENTRIES 7U
static uint8_t g_profile_storage[PROFILE_ENTRIES * 8U + 1U];

static uint8_t *make_profile(void)
{
    uint8_t *profile = g_profile_storage + 1;

    put_entry(profile, 0U, NEON_FX1_ISP_WB_GAIN_R_OFFSET, 0x20FU);
    put_entry(profile, 1U, NEON_FX1_ISP_COMMON_BAYER_OFFSET, 3U);      /* Owned: skipped. */
    put_entry(profile, 2U, NEON_FX1_ISP_LSC_LOAD_CTRL_OFFSET, 0x101U);
    put_entry(profile, 3U, NEON_FX1_ISP_LSC_COEF_DATA_OFFSET, 0x66666U);
    put_entry(profile, 4U, NEON_FX1_ISP_LSC_COEF_DATA_OFFSET, 0x67AE1U);
    put_entry(profile, 5U, NEON_FX1_ISP_RESIZER_CTRL_OFFSET, 0x2DU);   /* Owned: the output size is the HAL's. */
    put_entry(profile, 6U, NEON_FX1_ISP_AEC_CTRL_OFFSET, 0x3U);
    return profile;
}

static uint32_t expected_hash(void)
{
    static const uint32_t written[][2] = {
        {NEON_FX1_ISP_WB_GAIN_R_OFFSET, 0x20FU},
        {NEON_FX1_ISP_LSC_LOAD_CTRL_OFFSET, 0x101U},
        {NEON_FX1_ISP_LSC_COEF_DATA_OFFSET, 0x66666U},
        {NEON_FX1_ISP_LSC_COEF_DATA_OFFSET, 0x67AE1U},
        {NEON_FX1_ISP_AEC_CTRL_OFFSET, 0x3U},
    };
    uint32_t hash = 2166136261U;
    uint32_t i;

    for (i = 0U; i < 5U; i++) {
        hash = fnv_word(hash, written[i][0]);
        hash = fnv_word(hash, written[i][1]);
    }
    return hash;
}

static void test_profile_check(void)
{
    uint8_t *profile = make_profile();
    neon_isp_profile_info_t info;
    uint8_t bad[8];

    CHECK_EQ(neon_isp_check_profile(profile, PROFILE_ENTRIES * 8U, &info), NEON_OK);
    CHECK_EQ(info.entries, 7U);
    CHECK_EQ(info.written, 5U);
    CHECK_EQ(info.skipped, 2U);
    CHECK_EQ(neon_isp_check_profile(profile, PROFILE_ENTRIES * 8U, NULL), NEON_OK);

    CHECK_EQ(neon_isp_check_profile(NULL, 8U, &info), NEON_INVALID_PARAM);
    CHECK_EQ(neon_isp_check_profile(profile, 0U, &info), NEON_INVALID_PARAM);
    CHECK_EQ(neon_isp_check_profile(profile, 12U, &info), NEON_INVALID_PARAM); /* Not whole entries. */

    /* Entries that make the whole profile wrong. */
    put_entry(bad, 0U, 2U, 0U);                                   /* Not on a register boundary. */
    CHECK_EQ(neon_isp_check_profile(bad, 8U, &info), NEON_INVALID_PARAM);
    put_entry(bad, 0U, 0x0000FFFCU, 0U);                          /* Beyond the register map. */
    CHECK_EQ(neon_isp_check_profile(bad, 8U, &info), NEON_INVALID_PARAM);
    put_entry(bad, 0U, NEON_FX1_ISP_COMMON_VER_ID_OFFSET, 0U);    /* Read only. */
    CHECK_EQ(neon_isp_check_profile(bad, 8U, &info), NEON_INVALID_PARAM);
    put_entry(bad, 0U, NEON_FX1_ISP_COMMON_IRQ_STATUS_OFFSET, 0xFFFFFFFFU); /* Would clear interrupts. */
    CHECK_EQ(neon_isp_check_profile(bad, 8U, &info), NEON_INVALID_PARAM);
    put_entry(bad, 0U, NEON_FX1_ISP_AEC_STATUS_OFFSET, 0U);       /* Mixed, with write-one-to-clear bits. */
    CHECK_EQ(neon_isp_check_profile(bad, 8U, &info), NEON_INVALID_PARAM);

    /* A whole register the HAL owns is skipped, not refused. */
    put_entry(bad, 0U, NEON_FX1_ISP_ODMA_BUF_FREE_OFFSET, 0xFU);
    CHECK_EQ(neon_isp_check_profile(bad, 8U, &info), NEON_OK);
    CHECK_EQ(info.written, 0U);
    CHECK_EQ(info.skipped, 1U);
}

static void test_profile_load(void)
{
    neon_isp_handle_t isp;
    neon_isp_config_t config;
    neon_isp_profile_info_t info;
    fx1_isp_sim_counters_t counters;
    uint8_t *profile = make_profile();
    uint8_t refused[PROFILE_ENTRIES * 8U];

    CHECK_EQ(neon_isp_load_profile(NULL, profile, PROFILE_ENTRIES * 8U, &info), NEON_INVALID_PARAM);

    /* Into a fresh handle, before any configuration. */
    CHECK_EQ(neon_isp_init(&isp), NEON_OK);
    CHECK_EQ(neon_isp_load_profile(&isp, profile, PROFILE_ENTRIES * 8U, &info), NEON_OK);
    CHECK_EQ(info.written, 5U);
    fx1_isp_sim_get_counters(&isp, &counters);
    CHECK_EQ(counters.profile_writes, 5U);
    CHECK_EQ(counters.profile_hash, expected_hash()); /* The written entries, in file order. */
    CHECK_EQ(isp.state, NEON_ISP_STATE_IDLE);

    /* A refused profile writes nothing and keeps the configuration. running() initializes again, which
     * clears the sim's counters. */
    running(&isp);
    (void)memcpy(refused, profile, sizeof(refused));
    put_entry(refused, 6U, NEON_FX1_ISP_COMMON_VER_ID_OFFSET, 0U); /* The last entry is wrong. */
    CHECK_EQ(neon_isp_load_profile(&isp, profile, PROFILE_ENTRIES * 8U, &info), NEON_BUSY); /* Running. */
    CHECK_EQ(neon_isp_stop(&isp), NEON_OK);
    CHECK_EQ(neon_isp_load_profile(&isp, refused, sizeof(refused), &info), NEON_INVALID_PARAM);
    fx1_isp_sim_get_counters(&isp, &counters);
    CHECK_EQ(counters.profile_writes, 0U);
    CHECK_EQ(isp.state, NEON_ISP_STATE_CONFIGURED);

    /* An accepted profile leaves the ISP to be configured again before it starts. */
    queue(&isp, 0U, 1U);
    CHECK_EQ(neon_isp_load_profile(&isp, profile, PROFILE_ENTRIES * 8U, &info), NEON_OK);
    fx1_isp_sim_get_counters(&isp, &counters);
    CHECK_EQ(counters.profile_writes, 5U);
    CHECK_EQ(counters.profile_hash, expected_hash());
    CHECK_EQ(isp.state, NEON_ISP_STATE_IDLE);
    CHECK_EQ(neon_isp_start(&isp), NEON_ERROR);
    make_config(&config);
    CHECK_EQ(neon_isp_configure(&isp, &config), NEON_OK);
    CHECK_EQ(neon_isp_start(&isp), NEON_OK);
    queue(&isp, 0U, 1U); /* The frame queued before the load was forgotten: its slot is free. */
    CHECK_EQ(fx1_isp_sim_process(&isp, 1U), 1U);
}

int main(void)
{
    RUN(test_fx1_caps);
    RUN(test_arguments_and_defaults);
    RUN(test_configure_validation);
    RUN(test_core_follows_caps);
    RUN(test_states_and_start_stop);
    RUN(test_operation_order);
    RUN(test_frame_flow);
    RUN(test_output_order_across_wrap);
    RUN(test_input_queue_rules);
    RUN(test_output_starvation_and_underrun);
    RUN(test_statistics);
    RUN(test_statistics_generators_off);
    RUN(test_dynamic_controls);
    RUN(test_events);
    RUN(test_stop_discards_and_flushes);
    RUN(test_recover);
    RUN(test_failures_keep_state);
    RUN(test_reconfigure_forgets_inputs);
    RUN(test_profile_check);
    RUN(test_profile_load);
    return TEST_REPORT();
}
