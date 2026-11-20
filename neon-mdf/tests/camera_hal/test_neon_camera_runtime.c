/**
 * @file test_neon_camera_runtime.c
 * @brief Host tests of the camera runtime start/stop sequencing and 3A loop on the real MIPI, CSI and ISP HAL
 * (sim backends), with a fake sensor and 3A.
 */

#include "test_common.h"
#include "neon_camera_runtime.h"
#include "csi/platform/fx1/fx1_csi_simulation.h"
#include "isp/platform/fx1/fx1_isp_simulation.h"
#include "mipi/platform/fx1/fx1_mipi_simulation.h"
#include "fx1_registers.h"

#define POLLS 8U

#define WIDTH 64U
#define HEIGHT 32U
#define CSI_BUFFER_0 0x80000000UL
#define CSI_BUFFER_1 0x80010000UL
#define CSI_BUFFER_BYTES 4096U /* 64 pixels of 16 bits (128 B) per line, 32 lines. */
#define ISP_Y_BASE 0x90000000UL
#define ISP_UV_BASE 0x90100000UL
#define ISP_SLOT_GAP 0x2000UL

/* ----------------------------------------------------------------------------------------------- */
/* Fakes                                                                                            */
/* ----------------------------------------------------------------------------------------------- */
typedef struct {
    int step;                    /* Global call counter, to check the order. */
    int probe_step;
    int mode_step;
    int isp_configure_step;
    int isp_start_step;
    int stream_on_step;
    int stream_off_step;
    int isp_stop_step;
    int streaming;
    int fail_probe;
    int fail_stream;
    uint32_t latency;
    uint32_t exposure_lines;
    uint32_t exposure_frame;
    uint16_t analogue_gain;
    uint16_t digital_gain;
    /* What the HAL looked like when the ISP was configured and when the sensor started. */
    neon_csi_state_t csi_state_at_isp_configure;
    uint8_t lock_count_at_isp_configure;
    neon_isp_state_t isp_state_at_stream_on;
    /* What the 3A algorithm was given. */
    int algorithm_calls;
    uint32_t seen_frame_id;
    uint8_t seen_mean_luma;
    /* Objects that the fakes look at. */
    neon_isp_handle_t *isp;
    neon_csi_handle_t *csi;
    neon_mipi_handle_t *link;
} fake_t;

static fake_t g_fake;

static int sensor_probe(void *s) { fake_t *f = s; f->probe_step = ++f->step; return f->fail_probe; }
static int sensor_mode(void *s, uint32_t mode) { fake_t *f = s; (void)mode; f->mode_step = ++f->step; return 0; }
static int sensor_start(void *s)
{
    fake_t *f = s;

    f->stream_on_step = ++f->step;
    f->isp_state_at_stream_on = f->isp->state;
    if (f->fail_stream) return -1;
    f->streaming = 1;
    return 0;
}
static int sensor_stop(void *s) { fake_t *f = s; f->stream_off_step = ++f->step; f->streaming = 0; return 0; }
static int sensor_exposure(void *s, uint32_t lines, uint32_t frame)
{
    fake_t *f = s;

    f->exposure_lines = lines;
    f->exposure_frame = frame;
    return 0;
}
static int sensor_again(void *s, uint16_t gain, uint32_t frame) { fake_t *f = s; (void)frame; f->analogue_gain = gain; return 0; }
static int sensor_dgain(void *s, uint16_t gain, uint32_t frame) { fake_t *f = s; (void)frame; f->digital_gain = gain; return 0; }
static uint32_t sensor_latency(void *s) { return ((fake_t *)s)->latency; }

static const neon_camera_sensor_hal_ops_t SENSOR_OPS = {
    sensor_probe, sensor_mode, sensor_start, sensor_stop, sensor_exposure, sensor_again, sensor_dgain, sensor_latency,
};

/* The ISP is the real HAL on its sim backend. This observer only records when the runtime drove it. */
static void isp_observe(void *context, fx1_isp_sim_op_t operation)
{
    fake_t *f = context;

    switch (operation) {
    case FX1_ISP_SIM_OP_CONFIGURE:
        f->isp_configure_step = ++f->step;
        f->csi_state_at_isp_configure = f->csi->state;
        f->lock_count_at_isp_configure = f->link->lock_count;
        break;
    case FX1_ISP_SIM_OP_START:
        f->isp_start_step = ++f->step;
        break;
    case FX1_ISP_SIM_OP_STOP:
        f->isp_stop_step = ++f->step;
        break;
    default:
        break;
    }
}

static int algorithm_process(neon_camera_3a_t *a, const neon_isp_statistics_t *statistics, neon_camera_3a_controls_t *controls)
{
    (void)a;
    g_fake.algorithm_calls++;
    g_fake.seen_frame_id = statistics->frame_id;
    g_fake.seen_mean_luma = statistics->mean_luma;
    (void)memset(controls, 0, sizeof(*controls));
    controls->target_frame_id = 7U;
    controls->exposure_lines = 1000U;
    controls->analogue_gain_q8 = 512U;
    controls->digital_gain_q8 = 256U;
    controls->wb_red_gain_q8 = 300U;
    controls->wb_blue_gain_q8 = 400U;
    controls->apply_exposure = 1U;
    controls->apply_white_balance = 1U;
    return 0;
}

static const neon_camera_3a_ops_t ALGORITHM_OPS = {0, algorithm_process};

/* ----------------------------------------------------------------------------------------------- */
typedef struct {
    neon_mipi_handle_t link;
    neon_csi_handle_t csi;
    neon_isp_handle_t isp;
    neon_camera_3a_t algorithm;
    neon_camera_pipeline_t pipeline;
} rig_t;

static void make_isp_config(neon_isp_config_t *config)
{
    uint32_t i;

    neon_isp_get_default_config(config);
    config->width = WIDTH;
    config->height = HEIGHT;
    config->input_stride_bytes = WIDTH * 2U;
    config->y_stride_bytes = WIDTH;
    config->uv_stride_bytes = WIDTH;
    config->output_y_bytes = WIDTH * HEIGHT;
    config->output_uv_bytes = WIDTH * (HEIGHT / 2U);
    for (i = 0U; i < NEON_ISP_MAX_BUFFERS; i++) {
        config->output_y_addr[i] = (uint32_t)(ISP_Y_BASE + i * ISP_SLOT_GAP);
        config->output_uv_addr[i] = (uint32_t)(ISP_UV_BASE + i * ISP_SLOT_GAP);
    }
}

static void make_rig(rig_t *r)
{
    (void)memset(&g_fake, 0, sizeof(g_fake));
    g_fake.latency = 2U;
    (void)memset(r, 0, sizeof(*r));
    CHECK_EQ(neon_mipi_init(&r->link), NEON_OK);
    CHECK_EQ(neon_csi_init(&r->csi, &r->link), NEON_OK);
    CHECK_EQ(neon_isp_init(&r->isp), NEON_OK);
    g_fake.isp = &r->isp;
    g_fake.csi = &r->csi;
    g_fake.link = &r->link;
    fx1_isp_sim_set_observer(&r->isp, isp_observe, &g_fake);

    r->pipeline.sensor = &g_fake;
    r->pipeline.sensor_ops = &SENSOR_OPS;
    r->pipeline.mipi = &r->link;
    r->pipeline.mipi_config.direction = NEON_MIPI_DIRECTION_RX;
    r->pipeline.mipi_config.lane_count = 4U;
    r->pipeline.mipi_config.lane_rate_bps = 1000000000UL;
    r->pipeline.mipi_config.continuous_clock = true;
    r->pipeline.mipi_ready_timeout_polls = POLLS;
    r->pipeline.csi = &r->csi;
    neon_csi_get_default_config(&r->pipeline.csi_config);
    r->pipeline.csi_config.width = WIDTH;
    r->pipeline.csi_config.height = HEIGHT;
    r->pipeline.csi_config.dma.base_addr[0] = CSI_BUFFER_0;
    r->pipeline.csi_config.dma.base_addr[1] = CSI_BUFFER_1;
    r->pipeline.csi_config.dma.buffer_size_bytes = CSI_BUFFER_BYTES;
    r->pipeline.isp = &r->isp;
    make_isp_config(&r->pipeline.isp_config);
    r->pipeline.algorithm = &r->algorithm;
    r->pipeline.algorithm_ops = &ALGORITHM_OPS;
    r->pipeline.sensor_mode_id = 3U;
}

/* What the caller's frame loop does for one frame: hand a receiver buffer to the ISP and let it run. */
static void push_frame(rig_t *r, uint32_t n)
{
    neon_isp_input_t input;

    (void)memset(&input, 0, sizeof(input));
    input.address = (uint32_t)(n % 2U == 0U ? CSI_BUFFER_0 : CSI_BUFFER_1);
    input.size_bytes = CSI_BUFFER_BYTES;
    input.tag = n;
    CHECK_EQ(neon_isp_queue_input(&r->isp, &input), NEON_OK);
    CHECK_EQ(fx1_isp_sim_process(&r->isp, 1U), 1U);
}

static void test_start_order(void)
{
    rig_t r;
    const neon_camera_runtime_ops_t *ops = neon_camera_runtime_reference_ops();

    make_rig(&r);
    CHECK_EQ(ops->start(&r.pipeline), 0);
    CHECK(g_fake.probe_step < g_fake.mode_step);
    CHECK(g_fake.mode_step < g_fake.isp_configure_step);
    CHECK(g_fake.isp_configure_step < g_fake.isp_start_step);
    CHECK(g_fake.isp_start_step < g_fake.stream_on_step);
    /* By the time the ISP is configured the link is ready and locked by a running receiver. */
    CHECK_EQ(g_fake.csi_state_at_isp_configure, NEON_CSI_STATE_RUNNING);
    CHECK_EQ(g_fake.lock_count_at_isp_configure, 1U);
    CHECK_EQ(g_fake.isp_state_at_stream_on, NEON_ISP_STATE_RUNNING); /* The sensor streams last. */
    CHECK_EQ(r.link.state, NEON_MIPI_STATE_READY);
    CHECK_EQ(r.csi.state, NEON_CSI_STATE_RUNNING);
    CHECK_EQ(r.isp.state, NEON_ISP_STATE_RUNNING);
    CHECK_EQ(r.algorithm.profile.control_latency_frames, 2U);
    CHECK_EQ(ops->start(&r.pipeline), -1); /* Already running: the link is taken. */
    CHECK_EQ(r.csi.state, NEON_CSI_STATE_RUNNING);
    CHECK_EQ(r.isp.state, NEON_ISP_STATE_RUNNING);
    CHECK(g_fake.streaming);
}

static void test_stop_and_restart(void)
{
    rig_t r;
    const neon_camera_runtime_ops_t *ops = neon_camera_runtime_reference_ops();

    make_rig(&r);
    CHECK_EQ(ops->start(&r.pipeline), 0);
    CHECK_EQ(ops->stop(&r.pipeline), 0);
    CHECK(g_fake.stream_off_step < g_fake.isp_stop_step); /* Reverse order. */
    CHECK_EQ(r.isp.state, NEON_ISP_STATE_CONFIGURED);
    CHECK_EQ(r.csi.state, NEON_CSI_STATE_CONFIGURED);
    CHECK_EQ(r.link.state, NEON_MIPI_STATE_RELEASED);
    CHECK_EQ(r.link.lock_count, 0U);
    CHECK(!g_fake.streaming);
    CHECK_EQ(ops->start(&r.pipeline), 0); /* Same objects, same configuration. */
    CHECK_EQ(r.isp.state, NEON_ISP_STATE_RUNNING);
    CHECK_EQ(r.csi.state, NEON_CSI_STATE_RUNNING);
    CHECK_EQ(ops->stop(&r.pipeline), 0);
    CHECK_EQ(ops->stop(&r.pipeline), -1); /* Not running. */
}

static void test_stop_releases_what_it_can(void)
{
    rig_t r;
    const neon_camera_runtime_ops_t *ops = neon_camera_runtime_reference_ops();

    make_rig(&r);
    CHECK_EQ(ops->start(&r.pipeline), 0);
    fx1_isp_sim_set_stop_fails(&r.isp, true);
    CHECK_EQ(ops->stop(&r.pipeline), -1);
    CHECK_EQ(r.isp.state, NEON_ISP_STATE_RUNNING);    /* The ISP says it may still be running... */
    CHECK_EQ(r.csi.state, NEON_CSI_STATE_CONFIGURED); /* ...and the failure does not leave the others running. */
    CHECK_EQ(r.link.state, NEON_MIPI_STATE_RELEASED);
}

static void test_start_failures_roll_back(void)
{
    rig_t r;
    const neon_camera_runtime_ops_t *ops = neon_camera_runtime_reference_ops();

    /* The sensor cannot be probed: no hardware was touched. */
    make_rig(&r);
    g_fake.fail_probe = -1;
    CHECK_EQ(ops->start(&r.pipeline), -1);
    CHECK_EQ(r.link.state, NEON_MIPI_STATE_RELEASED);

    /* The lanes never reach Stop State. */
    make_rig(&r);
    fx1_mipi_sim_set_stuck_lanes(&r.link, 0x2U);
    CHECK_EQ(ops->start(&r.pipeline), -1);
    CHECK_EQ(r.link.state, NEON_MIPI_STATE_RELEASED);
    CHECK_EQ(r.csi.state, NEON_CSI_STATE_IDLE);
    CHECK_EQ(g_fake.isp_configure_step, 0);

    /* The CSI configuration is invalid: the link is given back. */
    make_rig(&r);
    r.pipeline.csi_config.dma.base_addr[1] = 0U;
    CHECK_EQ(ops->start(&r.pipeline), -1);
    CHECK_EQ(r.link.state, NEON_MIPI_STATE_RELEASED);
    CHECK_EQ(g_fake.isp_configure_step, 0);

    /* The ISP configuration is invalid: the receiver is stopped and the link released. */
    make_rig(&r);
    r.pipeline.isp_config.width = 0U;
    CHECK_EQ(ops->start(&r.pipeline), -1);
    CHECK_EQ(r.isp.state, NEON_ISP_STATE_IDLE);
    CHECK_EQ(r.csi.state, NEON_CSI_STATE_CONFIGURED);
    CHECK_EQ(r.link.state, NEON_MIPI_STATE_RELEASED);
    CHECK_EQ(g_fake.isp_start_step, 0);
    CHECK_EQ(g_fake.stream_on_step, 0);

    /* The ISP cannot start: the receiver is stopped and the link released. */
    make_rig(&r);
    fx1_isp_sim_set_start_fails(&r.isp, true);
    CHECK_EQ(ops->start(&r.pipeline), -1);
    CHECK_EQ(r.isp.state, NEON_ISP_STATE_CONFIGURED);
    CHECK_EQ(r.csi.state, NEON_CSI_STATE_CONFIGURED);
    CHECK_EQ(r.link.state, NEON_MIPI_STATE_RELEASED);
    CHECK_EQ(g_fake.stream_on_step, 0);
    CHECK_EQ(g_fake.isp_stop_step, 0); /* It never ran, so there is nothing to stop. */

    /* The sensor cannot stream: the ISP, the receiver and the link are undone in reverse. */
    make_rig(&r);
    g_fake.fail_stream = -1;
    CHECK_EQ(ops->start(&r.pipeline), -1);
    CHECK(g_fake.isp_stop_step != 0);
    CHECK_EQ(r.isp.state, NEON_ISP_STATE_CONFIGURED);
    CHECK_EQ(r.csi.state, NEON_CSI_STATE_CONFIGURED);
    CHECK_EQ(r.link.state, NEON_MIPI_STATE_RELEASED);
    CHECK_EQ(r.link.lock_count, 0U);

    /* A sensor that reports no control latency is not usable by the 3A loop. */
    make_rig(&r);
    g_fake.latency = 0U;
    CHECK_EQ(ops->start(&r.pipeline), -1);
    CHECK(g_fake.stream_off_step != 0);
    CHECK_EQ(r.isp.state, NEON_ISP_STATE_CONFIGURED);
    CHECK_EQ(r.link.state, NEON_MIPI_STATE_RELEASED);

    /* After a failed start the same objects start fine. */
    g_fake.latency = 2U;
    CHECK_EQ(ops->start(&r.pipeline), 0);
    CHECK_EQ(r.isp.state, NEON_ISP_STATE_RUNNING);
}

static void test_invalid_pipelines(void)
{
    rig_t r;
    rig_t other;
    const neon_camera_runtime_ops_t *ops = neon_camera_runtime_reference_ops();

    CHECK_EQ(ops->start(NULL), -1);
    make_rig(&r);
    r.pipeline.mipi_ready_timeout_polls = 0U;
    CHECK_EQ(ops->start(&r.pipeline), -1);
    make_rig(&r);
    r.pipeline.mipi = NULL;
    CHECK_EQ(ops->start(&r.pipeline), -1);
    make_rig(&r);
    r.pipeline.csi = NULL;
    CHECK_EQ(ops->start(&r.pipeline), -1);
    make_rig(&r);
    r.pipeline.isp = NULL;
    CHECK_EQ(ops->start(&r.pipeline), -1);
    CHECK_EQ(r.link.state, NEON_MIPI_STATE_RELEASED); /* Refused before anything was touched. */
    CHECK_EQ(g_fake.probe_step, 0);

    /* A receiver that is bound to another link would be started on a link that was never acquired here. */
    make_rig(&r);
    make_rig(&other);
    r.pipeline.csi = &other.csi;
    CHECK_EQ(ops->start(&r.pipeline), -1);
    CHECK_EQ(r.link.state, NEON_MIPI_STATE_RELEASED);
}

static void test_statistics_loop(void)
{
    rig_t r;
    const neon_camera_runtime_ops_t *ops = neon_camera_runtime_reference_ops();
    neon_isp_statistics_t given;
    uint16_t red = 0U;
    uint16_t blue = 0U;
    uint32_t first = 99U;

    make_rig(&r);
    CHECK_EQ(ops->start(&r.pipeline), 0);

    /* No frame has gone through the ISP: nothing to act on, and nothing is written. */
    CHECK_EQ(ops->on_statistics(&r.pipeline, NULL), 1);
    CHECK_EQ(g_fake.algorithm_calls, 0);
    CHECK_EQ(g_fake.exposure_lines, 0U);

    /* One frame later the statistics come from the ISP itself. */
    push_frame(&r, 0U);
    CHECK_EQ(ops->on_statistics(&r.pipeline, NULL), 0);
    CHECK_EQ(g_fake.algorithm_calls, 1);
    CHECK_EQ(g_fake.seen_frame_id, 1U);
    CHECK_EQ(g_fake.seen_mean_luma, 96U); /* The default sim scene. */
    CHECK_EQ(g_fake.exposure_lines, 1000U);
    CHECK_EQ(g_fake.exposure_frame, 7U);
    CHECK_EQ(g_fake.analogue_gain, 512U);
    CHECK_EQ(g_fake.digital_gain, 256U);

    /* The white balance reaches the ISP, and takes effect from the frame that starts after the write. */
    fx1_isp_sim_get_applied_controls(&r.isp, &red, &blue, &first);
    CHECK_EQ(first, 0U);
    push_frame(&r, 1U);
    fx1_isp_sim_get_applied_controls(&r.isp, &red, &blue, &first);
    CHECK_EQ(red, 300U);
    CHECK_EQ(blue, 400U);
    CHECK_EQ(first, 2U);

    /* The same result is not used twice, but a caller can pass statistics of its own. */
    CHECK_EQ(ops->on_statistics(&r.pipeline, NULL), 0); /* Frame 2 is new. */
    CHECK_EQ(ops->on_statistics(&r.pipeline, NULL), 1);
    CHECK_EQ(g_fake.algorithm_calls, 2);
    (void)memset(&given, 0, sizeof(given));
    given.frame_id = 40U;
    given.mean_luma = 10U;
    CHECK_EQ(ops->on_statistics(&r.pipeline, &given), 0);
    CHECK_EQ(g_fake.algorithm_calls, 3);
    CHECK_EQ(g_fake.seen_frame_id, 40U);
    CHECK_EQ(g_fake.seen_mean_luma, 10U);

    CHECK_EQ(ops->on_statistics(NULL, NULL), -1);
    r.pipeline.isp = NULL;
    CHECK_EQ(ops->on_statistics(&r.pipeline, &given), -1);
}

static void test_frames_flow_after_start(void)
{
    rig_t r;
    const neon_camera_runtime_ops_t *ops = neon_camera_runtime_reference_ops();
    neon_isp_output_t output;
    neon_isp_input_t back;

    make_rig(&r);
    CHECK_EQ(ops->start(&r.pipeline), 0);

    /* The runtime leaves a running ISP behind: a receiver buffer goes in and an NV12 frame comes out. */
    push_frame(&r, 0U);
    CHECK_EQ(neon_isp_reclaim_input(&r.isp, &back), NEON_OK);
    CHECK(back.processed);
    CHECK_EQ(back.address, CSI_BUFFER_0);
    CHECK_EQ(neon_isp_acquire_output(&r.isp, &output), NEON_OK);
    CHECK_EQ(output.y_addr, ISP_Y_BASE);
    CHECK_EQ(output.uv_addr, ISP_UV_BASE);
    CHECK_EQ(output.width, WIDTH);
    CHECK_EQ(output.height, HEIGHT);
    CHECK_EQ(output.tag, 0U);
    CHECK_EQ(neon_isp_release_output(&r.isp, output.buffer_index), NEON_OK);

    /* A frame that is still queued when the runtime stops is reported as not processed. */
    {
        neon_isp_input_t input;

        (void)memset(&input, 0, sizeof(input));
        input.address = (uint32_t)CSI_BUFFER_1;
        input.size_bytes = CSI_BUFFER_BYTES;
        CHECK_EQ(neon_isp_queue_input(&r.isp, &input), NEON_OK);
    }
    CHECK_EQ(ops->stop(&r.pipeline), 0);
    CHECK_EQ(neon_isp_reclaim_input(&r.isp, &back), NEON_OK);
    CHECK(!back.processed);
    CHECK_EQ(back.address, CSI_BUFFER_1);
}

/* ----------------------------------------------------------------------------------------------- */
/* neon_camera_runtime_init(): the same pipeline from one gathered configuration                    */
/* ----------------------------------------------------------------------------------------------- */
#define REGION_BASE 0x80000000UL

static void make_runtime_config(neon_camera_runtime_config_t *config, uint32_t width, uint32_t height)
{
    neon_camera_runtime_get_default_config(config);
    config->sensor = &g_fake;
    config->sensor_ops = &SENSOR_OPS;
    config->sensor_mode_id = 3U;
    config->width = width;
    config->height = height;
    config->lane_count = 4U;
    config->lane_rate_bps = 1000000000UL;
    config->memory_base = REGION_BASE;
    CHECK_EQ(neon_camera_runtime_memory_bytes(config, &config->memory_bytes), NEON_OK);
}

static void init_runtime(neon_camera_runtime_t *rt, const neon_camera_runtime_config_t *config)
{
    (void)memset(&g_fake, 0, sizeof(g_fake));
    g_fake.latency = 2U;
    CHECK_EQ(neon_camera_runtime_init(rt, config), NEON_OK);
    g_fake.isp = &rt->isp;
}

/* The caller's frame loop for one frame, from the addresses that init chose for the receiver. */
static void feed_frame(neon_camera_runtime_t *rt, uint32_t n)
{
    neon_isp_input_t input;

    (void)memset(&input, 0, sizeof(input));
    input.address = rt->pipeline.csi_config.dma.base_addr[n % 2U];
    input.size_bytes = rt->pipeline.csi_config.dma.buffer_size_bytes;
    input.tag = n;
    CHECK_EQ(neon_isp_queue_input(&rt->isp, &input), NEON_OK);
    CHECK_EQ(fx1_isp_sim_process(&rt->isp, 1U), 1U);
}

static void test_init_derives_configuration(void)
{
    neon_camera_runtime_t rt;
    neon_camera_runtime_config_t config;
    neon_camera_pipeline_t *p = &rt.pipeline;
    neon_isp_config_t hand;
    neon_csi_layout_t layout;
    uint32_t i;

    (void)memset(&g_fake, 0, sizeof(g_fake));
    make_runtime_config(&config, WIDTH, HEIGHT);
    /* 2 RAW buffers of 4096, and 4 NV12 frames of a 2048 byte Y plane and a 1024 byte UV plane. */
    CHECK_EQ(config.memory_bytes, 2U * 4096U + 4U * (2048U + 1024U));
    init_runtime(&rt, &config);

    CHECK(p->sensor == &g_fake);
    CHECK(p->sensor_ops == &SENSOR_OPS);
    CHECK_EQ(p->sensor_mode_id, 3U);
    CHECK(p->mipi == &rt.mipi && p->csi == &rt.csi && p->isp == &rt.isp && p->algorithm == &rt.algorithm);
    CHECK(rt.csi.mipi == &rt.mipi); /* The receiver is bound to the link. */
    CHECK(p->algorithm_ops == neon_camera_3a_reference_ops());
    CHECK_EQ(rt.algorithm.initialized, 1U);
    CHECK_EQ(rt.algorithm.profile.target_luma, 110U);
    CHECK_EQ(rt.mipi.state, NEON_MIPI_STATE_RELEASED);
    CHECK_EQ(rt.csi.state, NEON_CSI_STATE_IDLE);
    CHECK_EQ(rt.isp.state, NEON_ISP_STATE_IDLE);

    CHECK_EQ(p->mipi_config.direction, NEON_MIPI_DIRECTION_RX);
    CHECK_EQ(p->mipi_config.lane_count, 4U);
    CHECK_EQ(p->mipi_config.lane_rate_bps, 1000000000UL);
    CHECK(p->mipi_config.continuous_clock);
    CHECK_EQ(p->mipi_ready_timeout_polls, NEON_CAMERA_RUNTIME_DEFAULT_READY_POLLS);

    CHECK_EQ(p->csi_config.raw_format, NEON_CSI_RAW12);
    CHECK_EQ(p->csi_config.output_format, NEON_CSI_OUTPUT_RAW16);
    CHECK_EQ(p->csi_config.width, WIDTH);
    CHECK_EQ(p->csi_config.height, HEIGHT);
    CHECK_EQ(p->csi_config.dma.line_padding_bytes, 0U);
    CHECK_EQ(p->csi_config.dma.buffer_size_bytes, 4096U);
    CHECK_EQ(p->csi_config.dma.base_addr[0], REGION_BASE);
    CHECK_EQ(p->csi_config.dma.base_addr[1], REGION_BASE + 0x1000UL);

    /* The scalar fields agree with the ISP configuration that the earlier tests wrote by hand. */
    make_isp_config(&hand);
    CHECK_EQ(p->isp_config.width, hand.width);
    CHECK_EQ(p->isp_config.height, hand.height);
    CHECK_EQ(p->isp_config.raw_depth, hand.raw_depth);
    CHECK_EQ(p->isp_config.bayer_pattern, hand.bayer_pattern);
    CHECK_EQ(p->isp_config.input_stride_bytes, hand.input_stride_bytes);
    CHECK_EQ(p->isp_config.y_stride_bytes, hand.y_stride_bytes);
    CHECK_EQ(p->isp_config.uv_stride_bytes, hand.uv_stride_bytes);
    CHECK_EQ(p->isp_config.output_y_bytes, hand.output_y_bytes);
    CHECK_EQ(p->isp_config.output_uv_bytes, hand.output_uv_bytes);
    CHECK(p->isp_config.stats_aec && p->isp_config.stats_awb && p->isp_config.stats_af);
    for (i = 0U; i < rt.isp.caps.output_buffers; i++) {
        CHECK_EQ(p->isp_config.output_y_addr[i], REGION_BASE + 0x2000UL + i * 0xC00UL);
        CHECK_EQ(p->isp_config.output_uv_addr[i], p->isp_config.output_y_addr[i] + 0x800UL);
    }

    /* What the receiver writes is what the ISP reads. */
    CHECK_EQ(neon_csi_calculate_layout(&p->csi_config, &layout), NEON_OK);
    CHECK_EQ(layout.line_pitch_bytes, p->isp_config.input_stride_bytes);
    CHECK_EQ(layout.frame_bytes, p->csi_config.dma.buffer_size_bytes);
}

static void test_init_runs_the_pipeline(void)
{
    neon_camera_runtime_t rt;
    neon_camera_runtime_config_t config;
    const neon_camera_runtime_ops_t *ops = neon_camera_runtime_reference_ops();
    neon_isp_output_t output;
    neon_isp_input_t back;
    uint16_t red = 0U;
    uint16_t blue = 0U;
    uint32_t first = 0U;

    make_runtime_config(&config, WIDTH, HEIGHT);
    init_runtime(&rt, &config);
    CHECK_EQ(ops->start(&rt.pipeline), 0); /* No hand written configuration anywhere. */
    CHECK_EQ(rt.mipi.state, NEON_MIPI_STATE_READY);
    CHECK_EQ(rt.csi.state, NEON_CSI_STATE_RUNNING);
    CHECK_EQ(rt.isp.state, NEON_ISP_STATE_RUNNING);
    CHECK_EQ(g_fake.isp_state_at_stream_on, NEON_ISP_STATE_RUNNING);
    CHECK_EQ(rt.algorithm.profile.control_latency_frames, 2U);
    CHECK_EQ(ops->on_statistics(&rt.pipeline, NULL), 1); /* Nothing has gone through the ISP yet. */

    feed_frame(&rt, 0U);
    CHECK_EQ(neon_isp_reclaim_input(&rt.isp, &back), NEON_OK);
    CHECK_EQ(back.address, rt.pipeline.csi_config.dma.base_addr[0]);
    CHECK_EQ(neon_isp_acquire_output(&rt.isp, &output), NEON_OK);
    CHECK_EQ(output.y_addr, rt.pipeline.isp_config.output_y_addr[0]);
    CHECK_EQ(output.uv_addr, rt.pipeline.isp_config.output_uv_addr[0]);
    CHECK_EQ(neon_isp_release_output(&rt.isp, output.buffer_index), NEON_OK);

    /* The reference algorithm sees a scene darker (96) than its target (110) and a green heavy balance. */
    CHECK_EQ(ops->on_statistics(&rt.pipeline, NULL), 0);
    CHECK_EQ(g_fake.exposure_lines, 404U);
    CHECK_EQ(g_fake.exposure_frame, 3U); /* Frame 1 plus the sensor's latency of 2. */
    feed_frame(&rt, 1U);
    fx1_isp_sim_get_applied_controls(&rt.isp, &red, &blue, &first);
    CHECK_EQ(red, 279U);
    CHECK_EQ(blue, 219U);
    CHECK_EQ(first, 2U);

    CHECK_EQ(ops->stop(&rt.pipeline), 0);
    CHECK_EQ(neon_camera_runtime_deinit(&rt), NEON_OK);
}

static void test_init_memory(void)
{
    neon_camera_runtime_t rt;
    neon_camera_runtime_config_t config;
    uint32_t need = 0U;

    (void)memset(&g_fake, 0, sizeof(g_fake));
    make_runtime_config(&config, WIDTH, HEIGHT);
    need = config.memory_bytes;
    CHECK_EQ(need, 20480U);
    CHECK_EQ(neon_camera_runtime_memory_bytes(&config, NULL), NEON_INVALID_PARAM);
    CHECK_EQ(neon_camera_runtime_memory_bytes(NULL, &need), NEON_INVALID_PARAM);

    config.memory_bytes = need - 1U;
    CHECK_EQ(neon_camera_runtime_init(&rt, &config), NEON_INVALID_PARAM);

    /* A base that is not aligned moves up to the next 16 bytes, and the region must have room for that. */
    config.memory_base = REGION_BASE + 4UL;
    config.memory_bytes = need + 11U;
    CHECK_EQ(neon_camera_runtime_init(&rt, &config), NEON_INVALID_PARAM);
    config.memory_bytes = need + 12U;
    init_runtime(&rt, &config);
    CHECK_EQ(rt.pipeline.csi_config.dma.base_addr[0], REGION_BASE + 0x10UL);

    /* The region may end exactly at the top of the 32-bit address space, but not cross it. */
    config.memory_base = (uint32_t)(0x100000000ULL - need);
    config.memory_bytes = need;
    init_runtime(&rt, &config);
    CHECK_EQ((uint64_t)rt.pipeline.isp_config.output_uv_addr[rt.isp.caps.output_buffers - 1U] + 1024U, 0x100000000ULL);
    config.memory_base += 0x10U;
    CHECK_EQ(neon_camera_runtime_init(&rt, &config), NEON_INVALID_PARAM);
}

static void test_init_rejections(void)
{
    neon_camera_runtime_t rt;
    neon_camera_runtime_config_t config;
    uint32_t bytes = 0U;

    (void)memset(&g_fake, 0, sizeof(g_fake));
    make_runtime_config(&config, WIDTH, HEIGHT);
    CHECK_EQ(neon_camera_runtime_init(NULL, &config), NEON_INVALID_PARAM);
    CHECK_EQ(neon_camera_runtime_init(&rt, NULL), NEON_INVALID_PARAM);

    config.sensor = NULL;
    CHECK_EQ(neon_camera_runtime_init(&rt, &config), NEON_INVALID_PARAM);
    make_runtime_config(&config, WIDTH, HEIGHT);
    config.sensor_ops = NULL;
    CHECK_EQ(neon_camera_runtime_init(&rt, &config), NEON_INVALID_PARAM);
    make_runtime_config(&config, WIDTH, HEIGHT);
    config.lane_count = 0U;
    CHECK_EQ(neon_camera_runtime_init(&rt, &config), NEON_INVALID_PARAM);
    make_runtime_config(&config, WIDTH, HEIGHT);
    config.lane_rate_bps = 0U;
    CHECK_EQ(neon_camera_runtime_init(&rt, &config), NEON_INVALID_PARAM);

    make_runtime_config(&config, WIDTH, HEIGHT);
    config.width = 0U;
    CHECK_EQ(neon_camera_runtime_memory_bytes(&config, &bytes), NEON_INVALID_PARAM);
    config.width = 65536U; /* Does not fit the 16-bit geometry fields. */
    CHECK_EQ(neon_camera_runtime_memory_bytes(&config, &bytes), NEON_INVALID_PARAM);
    config.width = WIDTH;
    config.height = 0U;
    CHECK_EQ(neon_camera_runtime_memory_bytes(&config, &bytes), NEON_INVALID_PARAM);
    config.height = HEIGHT;
    config.width = 33U; /* The receiver packs two RAW12 pixels per three bytes. */
    CHECK_EQ(neon_camera_runtime_memory_bytes(&config, &bytes), NEON_INVALID_PARAM);
    config.width = WIDTH;
    config.raw_depth = (neon_isp_raw_depth_t)8;
    CHECK_EQ(neon_camera_runtime_memory_bytes(&config, &bytes), NEON_INVALID_PARAM);

    /* The receiver right-aligns RAW10 and the ISP reads it left-aligned: refused instead of a dark image. */
    config.raw_depth = NEON_ISP_RAW10;
    CHECK_EQ(neon_camera_runtime_memory_bytes(&config, &bytes), NEON_NOT_SUPPORTED);
    config.memory_bytes = 0x100000U;
    CHECK_EQ(neon_camera_runtime_init(&rt, &config), NEON_NOT_SUPPORTED);
}

static void test_init_pads_the_pitch(void)
{
    neon_camera_runtime_t rt;
    neon_camera_runtime_config_t config;
    const neon_camera_runtime_ops_t *ops = neon_camera_runtime_reference_ops();

    /* 1922 pixels are 3844 bytes, which the DMA cannot read in place: the receiver pads each line. */
    make_runtime_config(&config, 1922U, 32U);
    init_runtime(&rt, &config);
    CHECK_EQ(rt.pipeline.csi_config.dma.line_padding_bytes, 12U);
    CHECK_EQ(rt.pipeline.isp_config.input_stride_bytes, 3856U);
    CHECK_EQ(rt.pipeline.csi_config.dma.buffer_size_bytes, 3856U * 32U);
    CHECK_EQ(rt.pipeline.isp_config.y_stride_bytes, 1936U);
    CHECK_EQ(rt.pipeline.isp_config.uv_stride_bytes, 1936U);
    /* The real receiver and ISP accept what was derived. */
    CHECK_EQ(ops->start(&rt.pipeline), 0);
    CHECK_EQ(ops->stop(&rt.pipeline), 0);
}

static void test_init_leaves_hardware_rules_to_start(void)
{
    neon_camera_runtime_t rt;
    neon_camera_runtime_config_t config;
    const neon_camera_runtime_ops_t *ops = neon_camera_runtime_reference_ops();

    /* Three lanes: only the link knows that the chip has none. init() accepts, start() refuses and undoes. */
    make_runtime_config(&config, WIDTH, HEIGHT);
    config.lane_count = 3U;
    init_runtime(&rt, &config);
    CHECK_EQ(ops->start(&rt.pipeline), -1);
    CHECK_EQ(rt.mipi.state, NEON_MIPI_STATE_RELEASED);
    CHECK(g_fake.stream_on_step == 0);

    /* An odd height: only the ISP knows that NV12 needs an even one, and by then the receiver runs. */
    make_runtime_config(&config, WIDTH, 31U);
    init_runtime(&rt, &config);
    CHECK_EQ(ops->start(&rt.pipeline), -1);
    CHECK_EQ(rt.isp.state, NEON_ISP_STATE_IDLE);
    CHECK_EQ(rt.csi.state, NEON_CSI_STATE_CONFIGURED);
    CHECK_EQ(rt.mipi.state, NEON_MIPI_STATE_RELEASED);
    CHECK(g_fake.stream_on_step == 0);
}

static void test_init_deinit(void)
{
    neon_camera_runtime_t rt;
    neon_camera_runtime_config_t config;
    const neon_camera_runtime_ops_t *ops = neon_camera_runtime_reference_ops();

    CHECK_EQ(neon_camera_runtime_deinit(NULL), NEON_INVALID_PARAM);
    make_runtime_config(&config, WIDTH, HEIGHT);
    init_runtime(&rt, &config);
    CHECK_EQ(ops->start(&rt.pipeline), 0);
    CHECK_EQ(neon_camera_runtime_deinit(&rt), NEON_BUSY); /* Nothing is touched while it runs. */
    CHECK_EQ(rt.isp.state, NEON_ISP_STATE_RUNNING);
    CHECK_EQ(rt.csi.state, NEON_CSI_STATE_RUNNING);
    CHECK_EQ(ops->stop(&rt.pipeline), 0);
    CHECK_EQ(neon_camera_runtime_deinit(&rt), NEON_OK);
    CHECK(rt.pipeline.isp == NULL && rt.csi.mipi == NULL);
    init_runtime(&rt, &config); /* The storage can be used again. */
    CHECK_EQ(ops->start(&rt.pipeline), 0);
    CHECK_EQ(ops->stop(&rt.pipeline), 0);
}

/* ----------------------------------------------------------------------------------------------- */
/* Tuning binaries given to init()                                                                  */
/* ----------------------------------------------------------------------------------------------- */
static void put_entry(uint8_t *profile, uint32_t index, uint32_t offset, uint32_t value)
{
    uint32_t i;

    for (i = 0U; i < 4U; i++) {
        profile[index * 8U + i] = (uint8_t)(offset >> (8U * i));
        profile[index * 8U + 4U + i] = (uint8_t)(value >> (8U * i));
    }
}

/* Four entries in the shape of profile_D65.bin: two tuning registers and two the HAL owns. */
static uint8_t g_isp_profile[4U * 8U];

static void make_isp_profile(void)
{
    put_entry(g_isp_profile, 0U, NEON_FX1_ISP_COMMON_BAYER_OFFSET, 3U);  /* BGGR: the pipeline's RGGB wins. */
    put_entry(g_isp_profile, 1U, NEON_FX1_ISP_WB_GAIN_R_OFFSET, 0x20FU);
    put_entry(g_isp_profile, 2U, NEON_FX1_ISP_GAMMA_CTRL_OFFSET, 0x1U);
    put_entry(g_isp_profile, 3U, NEON_FX1_ISP_AEC_CONTEXT_ID_OFFSET, 0xA1U);
}

static void test_init_loads_tuning(void)
{
    neon_camera_runtime_t rt;
    neon_camera_runtime_config_t config;
    neon_camera_3a_profile_t tuning;
    const neon_camera_runtime_ops_t *ops = neon_camera_runtime_reference_ops();
    fx1_isp_sim_counters_t counters;

    make_isp_profile();
    neon_camera_3a_profile_default(&tuning);
    tuning.target_luma = 80U; /* Darker than the default 110. */
    make_runtime_config(&config, WIDTH, HEIGHT);
    config.isp_profile = g_isp_profile;
    config.isp_profile_bytes = sizeof(g_isp_profile);
    config.algorithm_tuning = &tuning;
    config.algorithm_tuning_bytes = sizeof(tuning);
    init_runtime(&rt, &config);

    /* init() read both binaries and touched no hardware. */
    CHECK_EQ(rt.isp_profile_info.entries, 4U);
    CHECK_EQ(rt.isp_profile_info.written, 2U);
    CHECK_EQ(rt.isp_profile_info.skipped, 2U);
    CHECK_EQ(rt.algorithm.profile.target_luma, 80U);
    fx1_isp_sim_get_counters(&rt.isp, &counters);
    CHECK_EQ(counters.profile_writes, 0U);

    /* start() writes the profile into the stopped ISP, then configures it: the HAL's settings win. */
    CHECK_EQ(ops->start(&rt.pipeline), 0);
    fx1_isp_sim_get_counters(&rt.isp, &counters);
    CHECK_EQ(counters.profile_writes, 2U);
    CHECK_EQ(counters.configures, 1U);
    CHECK_EQ(rt.isp.state, NEON_ISP_STATE_RUNNING);

    /* The 3A loop runs on the loaded tuning: a scene of 96 is brighter than 80, so exposure goes down. */
    feed_frame(&rt, 0U);
    CHECK_EQ(ops->on_statistics(&rt.pipeline, NULL), 0);
    CHECK_EQ(g_fake.exposure_lines, 395U);

    /* Every start writes it again: the registers of a stopped ISP are not assumed to be kept. */
    CHECK_EQ(ops->stop(&rt.pipeline), 0);
    CHECK_EQ(ops->start(&rt.pipeline), 0);
    fx1_isp_sim_get_counters(&rt.isp, &counters);
    CHECK_EQ(counters.profile_writes, 4U);
    CHECK_EQ(ops->stop(&rt.pipeline), 0);
}

static void test_init_refuses_bad_tuning(void)
{
    neon_camera_runtime_t rt;
    neon_camera_runtime_config_t config;
    neon_camera_3a_profile_t tuning;
    neon_camera_3a_ops_t no_loader = {0, 0};

    make_isp_profile();
    neon_camera_3a_profile_default(&tuning);
    (void)memset(&g_fake, 0, sizeof(g_fake));

    /* A pointer without a size, or a size without a pointer. */
    make_runtime_config(&config, WIDTH, HEIGHT);
    config.isp_profile = g_isp_profile;
    CHECK_EQ(neon_camera_runtime_init(&rt, &config), NEON_INVALID_PARAM);
    make_runtime_config(&config, WIDTH, HEIGHT);
    config.isp_profile_bytes = 8U;
    CHECK_EQ(neon_camera_runtime_init(&rt, &config), NEON_INVALID_PARAM);
    make_runtime_config(&config, WIDTH, HEIGHT);
    config.algorithm_tuning_bytes = sizeof(tuning);
    CHECK_EQ(neon_camera_runtime_init(&rt, &config), NEON_INVALID_PARAM);

    /* An ISP profile that does not fit the chip's register map is refused before anything is set up. */
    make_runtime_config(&config, WIDTH, HEIGHT);
    put_entry(g_isp_profile, 3U, NEON_FX1_ISP_COMMON_IRQ_STATUS_OFFSET, 0xFFFFFFFFU);
    config.isp_profile = g_isp_profile;
    config.isp_profile_bytes = sizeof(g_isp_profile);
    (void)memset(&rt, 0xA5, sizeof(rt));
    CHECK_EQ(neon_camera_runtime_init(&rt, &config), NEON_INVALID_PARAM);
    config.isp_profile_bytes = 12U; /* Not whole entries. */
    CHECK_EQ(neon_camera_runtime_init(&rt, &config), NEON_INVALID_PARAM);

    /* The 3A tuning replaces the profile structure, needs a loader, and must be what the loader expects. */
    make_runtime_config(&config, WIDTH, HEIGHT);
    config.algorithm_tuning = &tuning;
    config.algorithm_tuning_bytes = sizeof(tuning);
    config.profile = &tuning;
    CHECK_EQ(neon_camera_runtime_init(&rt, &config), NEON_INVALID_PARAM);
    config.profile = NULL;
    config.algorithm_ops = &no_loader;
    CHECK_EQ(neon_camera_runtime_init(&rt, &config), NEON_NOT_SUPPORTED);
    config.algorithm_ops = NULL;
    config.algorithm_tuning_bytes = sizeof(tuning) - 1U; /* The reference loader takes exactly one structure. */
    (void)memset(&rt, 0xA5, sizeof(rt));
    CHECK_EQ(neon_camera_runtime_init(&rt, &config), NEON_INVALID_PARAM);
    CHECK(rt.pipeline.isp == NULL && rt.csi.mipi == NULL); /* Nothing left half initialized. */
}

static void test_start_refuses_bad_isp_profile(void)
{
    rig_t r;
    const neon_camera_runtime_ops_t *ops = neon_camera_runtime_reference_ops();
    uint8_t bad[8];

    /* A hand-built pipeline is not checked by init(): start() refuses the profile and undoes the receiver. */
    make_rig(&r);
    put_entry(bad, 0U, NEON_FX1_ISP_COMMON_VER_ID_OFFSET, 0U);
    r.pipeline.isp_profile = bad;
    r.pipeline.isp_profile_bytes = sizeof(bad);
    CHECK_EQ(ops->start(&r.pipeline), -1);
    CHECK_EQ(g_fake.isp_configure_step, 0);
    CHECK_EQ(r.isp.state, NEON_ISP_STATE_IDLE);
    CHECK_EQ(r.csi.state, NEON_CSI_STATE_CONFIGURED);
    CHECK_EQ(r.link.state, NEON_MIPI_STATE_RELEASED);
    CHECK_EQ(g_fake.stream_on_step, 0);
}

/* ----------------------------------------------------------------------------------------------- */
/* Findings of the correctness review                                                               */
/* ----------------------------------------------------------------------------------------------- */
/* The statistics generators publish separately. A newer focus result alone must not run the 3A loop a
 * second time on an exposure result it already used: that would apply one correction twice. */
static void test_statistics_loop_once_per_exposure_result(void)
{
    neon_camera_runtime_t rt;
    neon_camera_runtime_config_t config;
    const neon_camera_runtime_ops_t *ops = neon_camera_runtime_reference_ops();
    uint32_t lines;

    make_runtime_config(&config, WIDTH, HEIGHT);
    init_runtime(&rt, &config);
    CHECK_EQ(ops->start(&rt.pipeline), 0);
    feed_frame(&rt, 0U);
    CHECK_EQ(ops->on_statistics(&rt.pipeline, NULL), 0);
    lines = g_fake.exposure_lines;

    fx1_isp_sim_hold_statistics(&rt.isp, 0x3U); /* Exposure and white balance lag; focus moves on. */
    feed_frame(&rt, 1U);
    CHECK_EQ(ops->on_statistics(&rt.pipeline, NULL), 1);
    CHECK_EQ(g_fake.exposure_lines, lines); /* Not corrected twice from frame 1. */

    fx1_isp_sim_hold_statistics(&rt.isp, 0U);
    feed_frame(&rt, 0U);
    CHECK_EQ(ops->on_statistics(&rt.pipeline, NULL), 0); /* Frame 3 is new for exposure too. */
    CHECK(g_fake.exposure_lines != lines);

    /* After a restart the ISP counts from frame 1 again: that is not mistaken for an old result. */
    CHECK_EQ(ops->stop(&rt.pipeline), 0);
    CHECK_EQ(ops->start(&rt.pipeline), 0);
    feed_frame(&rt, 0U);
    CHECK_EQ(ops->on_statistics(&rt.pipeline, NULL), 0);
    CHECK_EQ(ops->stop(&rt.pipeline), 0);
}

/* A scene too dark for the 8-bit mean must raise exposure, not stall the loop with an error. */
static void test_statistics_loop_dark_scene(void)
{
    neon_camera_runtime_t rt;
    neon_camera_runtime_config_t config;
    const neon_camera_runtime_ops_t *ops = neon_camera_runtime_reference_ops();
    fx1_isp_sim_scene_t dark = {0U, 0U, 0U, 0U, 0U, 0U, 0U};
    uint16_t red = 0U;
    uint16_t blue = 0U;
    uint32_t first = 0U;

    make_runtime_config(&config, WIDTH, HEIGHT);
    init_runtime(&rt, &config);
    CHECK_EQ(ops->start(&rt.pipeline), 0);
    fx1_isp_sim_set_scene(&rt.isp, &dark);
    feed_frame(&rt, 0U);
    CHECK_EQ(ops->on_statistics(&rt.pipeline, NULL), 0);
    CHECK_EQ(g_fake.exposure_lines, 428U); /* 400 + 110 * 25% + 1. */
    feed_frame(&rt, 1U);
    CHECK_EQ(ops->on_statistics(&rt.pipeline, NULL), 0);
    CHECK_EQ(g_fake.exposure_lines, 456U); /* Keeps climbing. */
    /* No colour to balance: the gains are left alone rather than divided by zero. */
    fx1_isp_sim_get_applied_controls(&rt.isp, &red, &blue, &first);
    CHECK_EQ(first, 0U);
    CHECK_EQ(ops->stop(&rt.pipeline), 0);
}

int main(void)
{
    RUN(test_start_order);
    RUN(test_stop_and_restart);
    RUN(test_stop_releases_what_it_can);
    RUN(test_start_failures_roll_back);
    RUN(test_invalid_pipelines);
    RUN(test_statistics_loop);
    RUN(test_frames_flow_after_start);
    RUN(test_init_derives_configuration);
    RUN(test_init_runs_the_pipeline);
    RUN(test_init_memory);
    RUN(test_init_rejections);
    RUN(test_init_pads_the_pitch);
    RUN(test_init_leaves_hardware_rules_to_start);
    RUN(test_init_deinit);
    RUN(test_init_loads_tuning);
    RUN(test_init_refuses_bad_tuning);
    RUN(test_start_refuses_bad_isp_profile);
    RUN(test_statistics_loop_once_per_exposure_result);
    RUN(test_statistics_loop_dark_scene);
    return TEST_REPORT();
}
