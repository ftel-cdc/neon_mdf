/**
 * @file test_neon_csi.c
 * @brief Host tests of the CSI-2 receiver HAL on the in-RAM backends, together with the MIPI link.
 */

#include "test_common.h"
#include "csi/neon_csi.h"
#include "csi/platform/fx1/fx1_csi_simulation.h"
#include "mipi/neon_mipi.h"
#include "mipi/platform/fx1/fx1_mipi_simulation.h"

#define POLLS 8U
#define BUF0 0x80000000UL
#define BUF1 0x80010000UL
#define META0 0x80020000UL
#define META1 0x80021000UL
#define WIDTH 64U
#define HEIGHT 32U
#define FRAME_BYTES 4096UL /* RAW16: 128 bytes per line, 32 lines. */

typedef struct {
    neon_mipi_handle_t link;
    neon_csi_handle_t csi;
    neon_csi_config_t config;
} env_t;

static void make_config(neon_csi_config_t *config, bool metadata, neon_csi_error_policy_t policy)
{
    neon_csi_get_default_config(config);
    config->width = WIDTH;
    config->height = HEIGHT;
    config->dma.base_addr[0] = BUF0;
    config->dma.base_addr[1] = BUF1;
    config->dma.buffer_size_bytes = FRAME_BYTES;
    config->dma.error_policy = policy;
    if (metadata) {
        config->metadata.enable = true;
        config->metadata.data_type = 0x12U;
        config->metadata.extract_enable = true;
        config->metadata.extract[0].length_bytes = 4U;
        config->metadata.extract[0].byte_offset = 0x10U;
        config->metadata.extract[1].length_bytes = 2U;
        config->metadata.extract[1].byte_offset = 0x20U;
        config->metadata.extract[1].line_index = 1U;
        config->metadata.record_id = 0x1234U;
        config->metadata.base_addr[0] = META0;
        config->metadata.base_addr[1] = META1;
        config->metadata.area_size_bytes = 256U;
    }
}

static void link_up(env_t *e)
{
    neon_mipi_config_t link_config = {NEON_MIPI_DIRECTION_RX, 4U, 1000000000UL, true};

    (void)neon_mipi_init(&e->link);
    CHECK_EQ(neon_mipi_acquire(&e->link, &link_config), NEON_OK);
    CHECK_EQ(neon_mipi_wait_ready(&e->link, POLLS), NEON_OK);
}

/* Link ready, receiver configured; not started. */
static void env_configured(env_t *e, bool metadata, neon_csi_error_policy_t policy)
{
    link_up(e);
    CHECK_EQ(neon_csi_init(&e->csi, &e->link), NEON_OK);
    make_config(&e->config, metadata, policy);
    CHECK_EQ(neon_csi_configure(&e->csi, &e->config), NEON_OK);
}

static void env_running(env_t *e, bool metadata, neon_csi_error_policy_t policy)
{
    env_configured(e, metadata, policy);
    CHECK_EQ(neon_csi_start(&e->csi), NEON_OK);
}

static fx1_csi_sim_frame_t frame_at(uint64_t timestamp_ns)
{
    fx1_csi_sim_frame_t frame;

    (void)memset(&frame, 0, sizeof(frame));
    frame.timestamp_ns = timestamp_ns;
    return frame;
}

static void push(env_t *e, uint64_t timestamp_ns)
{
    fx1_csi_sim_frame_t frame = frame_at(timestamp_ns);

    CHECK_EQ(fx1_csi_sim_push_frame(&e->csi, &frame), NEON_OK);
}

static void test_fx1_caps(void)
{
    neon_csi_caps_t caps;

    CHECK_EQ(neon_csi_get_caps(NULL), NEON_INVALID_PARAM);
    CHECK_EQ(neon_csi_get_caps(&caps), NEON_OK);
    CHECK_EQ(caps.buffer_count, 2U);
    CHECK_EQ(caps.vc_count, 2U);
    CHECK_EQ(caps.extract_slots, 2U);
    CHECK_EQ(caps.frame_id_bits, 14U);
    CHECK_EQ(caps.max_burst_beats, 256U);
    CHECK_EQ(caps.max_outstanding, 127U);
    CHECK_EQ(caps.max_qos, 15U);
    CHECK_EQ(caps.max_prot, 7U);
    CHECK_EQ(caps.max_line_padding_bytes, 255U);
    CHECK_EQ(caps.min_tpg_dimension, 8U);
    CHECK_EQ(caps.tpg_width_granularity, 4U);
    CHECK(caps.test_pattern);
    CHECK_EQ(caps.max_bytes_per_line, 0xFFFFU);
    CHECK_EQ(caps.dma_align_bytes, 16U);
    CHECK_EQ(caps.meta_record_min_bytes, 128U);
    CHECK_EQ(caps.supported_events, 0x00FFFFFFUL);
    CHECK((caps.supported_events & NEON_CSI_EVENT_LINK_RESET) == 0UL); /* Software event, on every chip. */
    CHECK(caps.buffer_count <= NEON_CSI_MAX_BUFFERS);
    CHECK(caps.vc_count <= NEON_CSI_MAX_VCS);
    CHECK(caps.extract_slots <= NEON_CSI_MAX_EXTRACT_SLOTS);
}

/*
 * The rules in neon_csi.c must come from the caps and not from FX1 numbers: a receiver bound to another
 * chip only has other caps. The handle holds a copy, so the test changes it.
 */
static void test_core_follows_caps(void)
{
    env_t e;
    neon_csi_config_t config;

    link_up(&e);
    CHECK_EQ(neon_csi_init(&e.csi, &e.link), NEON_OK);

    make_config(&config, false, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    e.csi.caps.max_burst_beats = 64U;
    config.dma.max_burst_beats = 128U;
    CHECK_EQ(neon_csi_configure(&e.csi, &config), NEON_INVALID_PARAM);
    config.dma.max_burst_beats = 64U;
    CHECK_EQ(neon_csi_configure(&e.csi, &config), NEON_OK);

    e.csi.caps.dma_align_bytes = 256U; /* The 128-byte line pitch is no longer a multiple. */
    CHECK_EQ(neon_csi_configure(&e.csi, &config), NEON_INVALID_PARAM);
    e.csi.caps.dma_align_bytes = 64U;
    CHECK_EQ(neon_csi_configure(&e.csi, &config), NEON_OK);

    e.csi.caps.max_bytes_per_line = 100U;
    CHECK_EQ(neon_csi_configure(&e.csi, &config), NEON_NOT_SUPPORTED);
    e.csi.caps.max_bytes_per_line = 0xFFFFU;

    e.csi.caps.max_line_padding_bytes = 8U;
    config.dma.line_padding_bytes = 16U;
    CHECK_EQ(neon_csi_configure(&e.csi, &config), NEON_INVALID_PARAM);
    config.dma.line_padding_bytes = 0U;
    e.csi.caps.max_line_padding_bytes = 255U;

    e.csi.caps.test_pattern = false;
    config.test_pattern.enable = true;
    config.test_pattern.width = WIDTH;
    config.test_pattern.height = HEIGHT;
    CHECK_EQ(neon_csi_configure(&e.csi, &config), NEON_NOT_SUPPORTED);
    config.test_pattern.enable = false;

    e.csi.caps.supported_events = NEON_CSI_EVENT_FRAME_RECEIVED;
    CHECK_EQ(neon_csi_configure(&e.csi, &config), NEON_INVALID_PARAM); /* The default mask asks for more. */
    config.event_mask = NEON_CSI_EVENT_FRAME_RECEIVED | NEON_CSI_EVENT_LINK_RESET;
    CHECK_EQ(neon_csi_configure(&e.csi, &config), NEON_OK);
    e.csi.caps.supported_events = 0x00FFFFFFUL;

    make_config(&config, true, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    e.csi.caps.extract_slots = 1U;
    CHECK_EQ(neon_csi_configure(&e.csi, &config), NEON_NOT_SUPPORTED); /* Slot 1 is used. */
    e.csi.caps.extract_slots = 2U;
    e.csi.caps.meta_record_min_bytes = 0U;
    CHECK_EQ(neon_csi_configure(&e.csi, &config), NEON_NOT_SUPPORTED); /* No metadata record on this chip. */
    e.csi.caps.meta_record_min_bytes = 512U;
    CHECK_EQ(neon_csi_configure(&e.csi, &config), NEON_INVALID_PARAM); /* Area of 256 bytes is too small. */
    e.csi.caps.meta_record_min_bytes = 128U;
    CHECK_EQ(neon_csi_configure(&e.csi, &config), NEON_OK);
}

static void test_chip_with_one_buffer_and_narrow_frame_id(void)
{
    env_t e;
    neon_csi_config_t config;
    neon_csi_frame_t frame;
    neon_csi_status_t status;
    fx1_csi_sim_frame_t in = frame_at(0U);

    link_up(&e);
    CHECK_EQ(neon_csi_init(&e.csi, &e.link), NEON_OK);
    e.csi.caps.buffer_count = 1U;
    e.csi.caps.vc_count = 1U;
    e.csi.caps.frame_id_bits = 4U;
    make_config(&config, true, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    config.dma.base_addr[1] = 0U; /* Unused: the chip has one buffer. */
    config.metadata.base_addr[1] = 0U;
    CHECK_EQ(neon_csi_configure(&e.csi, &config), NEON_OK);
    CHECK_EQ(neon_csi_start(&e.csi), NEON_OK);

    in.vc_mask = 3U; /* The second channel does not exist on this chip. */
    CHECK_EQ(fx1_csi_sim_push_frame(&e.csi, &in), NEON_OK);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_OK);
    CHECK_EQ(frame.buffer_index, 0U);
    CHECK(frame.vc[0].present);
    CHECK(!frame.vc[1].present);
    CHECK_EQ(fx1_csi_sim_push_frame(&e.csi, &in), NEON_BUSY); /* The only buffer is held by software. */
    (void)neon_csi_get_status(&e.csi, &status);
    CHECK_EQ(status.frame_drop_count, 1U);
    CHECK_EQ(neon_csi_release_frame(&e.csi, 1U), NEON_INVALID_PARAM); /* There is no buffer 1. */
    CHECK_EQ(neon_csi_release_frame(&e.csi, 0U), NEON_OK);

    /* Frame ids wrap at 16 here: the pick of the oldest frame follows frame_id_bits. */
    fx1_csi_sim_set_next_frame_id(&e.csi, 0x1FU);
    CHECK_EQ(fx1_csi_sim_push_frame(&e.csi, &in), NEON_OK);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_OK);
    CHECK_EQ(frame.frame_id, 0xFU);
    CHECK_EQ(neon_csi_release_frame(&e.csi, 0U), NEON_OK);
    CHECK_EQ(fx1_csi_sim_push_frame(&e.csi, &in), NEON_OK);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_OK);
    CHECK_EQ(frame.frame_id, 0U);
}

static void test_arguments_and_defaults(void)
{
    env_t e;
    neon_csi_config_t config;

    CHECK_EQ(neon_csi_init(NULL, &e.link), NEON_INVALID_PARAM);
    CHECK_EQ(neon_csi_init(&e.csi, NULL), NEON_INVALID_PARAM);
    neon_csi_get_default_config(NULL);
    neon_csi_get_default_config(&config);
    CHECK_EQ(config.raw_format, NEON_CSI_RAW12);
    CHECK_EQ(config.output_format, NEON_CSI_OUTPUT_RAW16);
    CHECK_EQ(config.dma.max_burst_beats, 16U);
    CHECK_EQ(config.dma.max_outstanding, 4U);
    CHECK_EQ(config.dma.error_policy, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    CHECK((config.event_mask & NEON_CSI_EVENT_FRAME_RECEIVED) != 0UL);
    CHECK_EQ(config.width, 0U);
    link_up(&e);
    CHECK_EQ(neon_csi_init(&e.csi, &e.link), NEON_OK);
    CHECK_EQ(e.csi.state, NEON_CSI_STATE_IDLE);
    CHECK_EQ(neon_csi_configure(NULL, &config), NEON_INVALID_PARAM);
    CHECK_EQ(neon_csi_start(&e.csi), NEON_ERROR);  /* Not configured. */
    CHECK_EQ(neon_csi_stop(&e.csi), NEON_ERROR);
    CHECK_EQ(neon_csi_deinit(&e.csi), NEON_OK);
}

static void test_layout(void)
{
    neon_csi_config_t config;
    neon_csi_layout_t layout;

    neon_csi_get_default_config(&config);
    config.width = 1920U;
    config.height = 1080U;
    CHECK_EQ(neon_csi_calculate_layout(&config, &layout), NEON_OK);
    CHECK_EQ(layout.bytes_per_line, 3840U);
    CHECK_EQ(layout.line_pitch_bytes, 3840U);
    CHECK_EQ(layout.frame_bytes, 4147200UL);
    CHECK_EQ(layout.sample_align, NEON_CSI_ALIGN_RIGHT);

    config.output_format = NEON_CSI_OUTPUT_PACKED;
    CHECK_EQ(neon_csi_calculate_layout(&config, &layout), NEON_OK);
    CHECK_EQ(layout.bytes_per_line, 2880U); /* 2 pixels in 3 bytes. */
    CHECK_EQ(layout.sample_align, NEON_CSI_ALIGN_NONE);
    config.raw_format = NEON_CSI_RAW10;
    CHECK_EQ(neon_csi_calculate_layout(&config, &layout), NEON_OK);
    CHECK_EQ(layout.bytes_per_line, 2400U); /* 4 pixels in 5 bytes. */

    config.width = 6U; /* Not a whole number of RAW10 packing units. */
    CHECK_EQ(neon_csi_calculate_layout(&config, &layout), NEON_INVALID_PARAM);
    config.raw_format = NEON_CSI_RAW12;
    config.width = 5U;
    CHECK_EQ(neon_csi_calculate_layout(&config, &layout), NEON_INVALID_PARAM);
    config.width = 0U;
    CHECK_EQ(neon_csi_calculate_layout(&config, &layout), NEON_INVALID_PARAM);
    config.width = 64U;
    config.height = 0U;
    CHECK_EQ(neon_csi_calculate_layout(&config, &layout), NEON_INVALID_PARAM);
    config.height = 8U;
    config.raw_format = (neon_csi_raw_format_t)8;
    CHECK_EQ(neon_csi_calculate_layout(&config, &layout), NEON_INVALID_PARAM);
    config.raw_format = NEON_CSI_RAW12;
    config.output_format = (neon_csi_output_format_t)7;
    CHECK_EQ(neon_csi_calculate_layout(&config, &layout), NEON_INVALID_PARAM);
    CHECK_EQ(neon_csi_calculate_layout(NULL, &layout), NEON_INVALID_PARAM);
    CHECK_EQ(neon_csi_calculate_layout(&config, NULL), NEON_INVALID_PARAM);

    /* The line pitch register is 16 bits wide, and a frame must fit in 32 bits. */
    config.output_format = NEON_CSI_OUTPUT_RAW16;
    config.width = 40000U;
    CHECK_EQ(neon_csi_calculate_layout(&config, &layout), NEON_NOT_SUPPORTED);
    config.output_format = NEON_CSI_OUTPUT_PACKED;
    config.width = 43690U; /* 65535 bytes per line. */
    config.height = 65535U;
    config.dma.line_padding_bytes = 255U;
    CHECK_EQ(neon_csi_calculate_layout(&config, &layout), NEON_NOT_SUPPORTED);

    neon_csi_get_default_config(&config);
    config.width = WIDTH;
    config.height = 8U;
    config.dma.line_padding_bytes = 16U;
    CHECK_EQ(neon_csi_calculate_layout(&config, &layout), NEON_OK);
    CHECK_EQ(layout.line_pitch_bytes, 144U);
    CHECK_EQ(layout.frame_bytes, 1152U);

    /* The test pattern replaces the sensor geometry, truncated to four pixels. */
    config.dma.line_padding_bytes = 0U;
    config.width = 1920U;
    config.height = 1080U;
    config.test_pattern.enable = true;
    config.test_pattern.pixel_format = NEON_CSI_RAW10;
    config.test_pattern.width = 66U;
    config.test_pattern.height = 8U;
    CHECK_EQ(neon_csi_calculate_layout(&config, &layout), NEON_OK);
    CHECK_EQ(layout.width, 64U);
    CHECK_EQ(layout.height, 8U);
    CHECK_EQ(layout.raw_format, NEON_CSI_RAW10);
    config.test_pattern.width = 6U;
    CHECK_EQ(neon_csi_calculate_layout(&config, &layout), NEON_INVALID_PARAM);
}

static void test_configure_validation(void)
{
    env_t e;
    neon_csi_config_t bad;

    /* A released link cannot be configured against. */
    (void)neon_mipi_init(&e.link);
    (void)neon_csi_init(&e.csi, &e.link);
    make_config(&e.config, false, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    CHECK_EQ(neon_csi_configure(&e.csi, &e.config), NEON_ERROR);

    link_up(&e);
    make_config(&bad, false, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    bad.dma.base_addr[0] |= 1U;
    CHECK_EQ(neon_csi_configure(&e.csi, &bad), NEON_INVALID_PARAM);
    make_config(&bad, false, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    bad.dma.base_addr[1] = 0U;
    CHECK_EQ(neon_csi_configure(&e.csi, &bad), NEON_INVALID_PARAM);
    make_config(&bad, false, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    bad.dma.base_addr[1] = BUF0 + 2048U; /* Overlaps the first buffer. */
    CHECK_EQ(neon_csi_configure(&e.csi, &bad), NEON_INVALID_PARAM);
    make_config(&bad, false, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    bad.dma.buffer_size_bytes = (uint32_t)FRAME_BYTES - 1U;
    CHECK_EQ(neon_csi_configure(&e.csi, &bad), NEON_INVALID_PARAM);
    make_config(&bad, false, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    bad.dma.error_policy = (neon_csi_error_policy_t)5;
    CHECK_EQ(neon_csi_configure(&e.csi, &bad), NEON_INVALID_PARAM);
    make_config(&bad, false, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    bad.dma.max_burst_beats = 0U;
    CHECK_EQ(neon_csi_configure(&e.csi, &bad), NEON_INVALID_PARAM);
    bad.dma.max_burst_beats = 257U;
    CHECK_EQ(neon_csi_configure(&e.csi, &bad), NEON_INVALID_PARAM);
    bad.dma.max_burst_beats = 256U;
    bad.dma.max_outstanding = 128U;
    CHECK_EQ(neon_csi_configure(&e.csi, &bad), NEON_INVALID_PARAM);
    bad.dma.max_outstanding = 127U;
    bad.dma.qos = 16U;
    CHECK_EQ(neon_csi_configure(&e.csi, &bad), NEON_INVALID_PARAM);
    bad.dma.qos = 15U;
    bad.dma.prot = 8U;
    CHECK_EQ(neon_csi_configure(&e.csi, &bad), NEON_INVALID_PARAM);
    make_config(&bad, false, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    bad.event_mask = 1UL << 30;
    CHECK_EQ(neon_csi_configure(&e.csi, &bad), NEON_INVALID_PARAM);

    /* A packed line of 2883 bytes cannot be consumed by the ISP input DMA. */
    make_config(&bad, false, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    bad.output_format = NEON_CSI_OUTPUT_PACKED;
    bad.width = 1922U;
    bad.height = 8U;
    bad.dma.buffer_size_bytes = 0x6000U;
    CHECK_EQ(neon_csi_configure(&e.csi, &bad), NEON_INVALID_PARAM);
    bad.dma.line_padding_bytes = 13U; /* 2896 is a multiple of 16. */
    CHECK_EQ(neon_csi_configure(&e.csi, &bad), NEON_OK);

    make_config(&bad, true, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    bad.metadata.enable = false;
    CHECK_EQ(neon_csi_configure(&e.csi, &bad), NEON_INVALID_PARAM); /* Extraction needs the record. */
    make_config(&bad, true, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    bad.metadata.extract[0].length_bytes = 5U;
    CHECK_EQ(neon_csi_configure(&e.csi, &bad), NEON_INVALID_PARAM);
    make_config(&bad, true, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    bad.metadata.area_size_bytes = 64U;
    CHECK_EQ(neon_csi_configure(&e.csi, &bad), NEON_INVALID_PARAM);
    make_config(&bad, true, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    bad.metadata.data_type = 0x40U;
    CHECK_EQ(neon_csi_configure(&e.csi, &bad), NEON_INVALID_PARAM);
    make_config(&bad, true, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    bad.metadata.base_addr[0] = BUF0 + 256U; /* Inside a RAW buffer. */
    CHECK_EQ(neon_csi_configure(&e.csi, &bad), NEON_INVALID_PARAM);
    make_config(&bad, true, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    bad.metadata.base_addr[1] = META0 + 256U; /* Overlaps the first area. */
    CHECK_EQ(neon_csi_configure(&e.csi, &bad), NEON_INVALID_PARAM);
    make_config(&bad, true, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    CHECK_EQ(neon_csi_configure(&e.csi, &bad), NEON_OK);
    CHECK_EQ(e.csi.state, NEON_CSI_STATE_CONFIGURED);
    CHECK_EQ(e.csi.layout.frame_bytes, FRAME_BYTES);
}

static void test_start_stop_and_link_lock(void)
{
    env_t e;
    fx1_csi_sim_counters_t counters;

    env_configured(&e, false, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    CHECK_EQ(neon_csi_start(&e.csi), NEON_OK);
    CHECK_EQ(e.csi.state, NEON_CSI_STATE_RUNNING);
    CHECK_EQ(e.link.lock_count, 1U);
    CHECK_EQ(neon_mipi_release(&e.link), NEON_LOCKED);
    CHECK_EQ(neon_csi_start(&e.csi), NEON_BUSY);
    CHECK_EQ(neon_csi_configure(&e.csi, &e.config), NEON_BUSY);
    CHECK_EQ(neon_csi_deinit(&e.csi), NEON_BUSY);
    CHECK_EQ(neon_csi_stop(&e.csi), NEON_OK);
    CHECK_EQ(e.csi.state, NEON_CSI_STATE_CONFIGURED);
    CHECK_EQ(e.link.lock_count, 0U);
    CHECK_EQ(neon_csi_stop(&e.csi), NEON_ERROR);
    CHECK_EQ(neon_csi_start(&e.csi), NEON_OK); /* Restart without configuring again. */
    CHECK_EQ(neon_csi_stop(&e.csi), NEON_OK);
    fx1_csi_sim_get_counters(&e.csi, &counters);
    CHECK_EQ(counters.starts, 2U);
    CHECK_EQ(counters.stops, 2U);

    /* The link is enabled but has not reached Stop State: MAS 9.1 steps 1 to 5 are not done. */
    (void)neon_mipi_release(&e.link);
    {
        neon_mipi_config_t link_config = {NEON_MIPI_DIRECTION_RX, 4U, 1000000000UL, true};

        fx1_mipi_sim_set_stuck_lanes(&e.link, 0x1U);
        (void)neon_mipi_acquire(&e.link, &link_config);
        CHECK_EQ(neon_mipi_wait_ready(&e.link, POLLS), NEON_TIMEOUT);
    }
    CHECK_EQ(neon_csi_start(&e.csi), NEON_ERROR);
    CHECK_EQ(e.csi.state, NEON_CSI_STATE_CONFIGURED);
    CHECK_EQ(e.link.lock_count, 0U);
    fx1_csi_sim_get_counters(&e.csi, &counters);
    CHECK_EQ(counters.starts, 2U); /* The backend was not started. */
}

static void test_frame_flow(void)
{
    env_t e;
    neon_csi_frame_t frame;
    fx1_csi_sim_frame_t in = frame_at(1000U);
    neon_csi_status_t status;
    uint32_t events;

    env_running(&e, false, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_BUSY);
    CHECK_EQ(neon_csi_acquire_frame(NULL, &frame), NEON_INVALID_PARAM);
    CHECK_EQ(fx1_csi_sim_push_frame(&e.csi, &in), NEON_OK);
    CHECK_EQ(neon_csi_get_events(&e.csi, &events), NEON_OK);
    CHECK((events & NEON_CSI_EVENT_FRAME_RECEIVED) != 0UL);
    CHECK((events & NEON_CSI_EVENT_BUFFER_VALID) != 0UL);

    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_OK);
    CHECK_EQ(frame.buffer_index, 0U);
    CHECK_EQ(frame.state, NEON_CSI_BUFFER_SW_VALID);
    CHECK_EQ(frame.base_addr, BUF0);
    CHECK_EQ(frame.frame_id, 0U);
    CHECK_EQ(frame.timestamp_ns, 1000U);
    CHECK(frame.context_valid);
    CHECK(frame.vc[0].present);
    CHECK(!frame.vc[1].present);
    CHECK_EQ(frame.vc[0].byte_count, 128U);
    CHECK_EQ(frame.vc[0].line_count, HEIGHT);
    CHECK_EQ(frame.layout.width, WIDTH);
    CHECK_EQ(frame.layout.frame_bytes, FRAME_BYTES);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_BUSY); /* Held by software already. */

    CHECK_EQ(neon_csi_release_frame(&e.csi, 1U), NEON_INVALID_PARAM); /* Never acquired. */
    CHECK_EQ(neon_csi_release_frame(&e.csi, 2U), NEON_INVALID_PARAM);
    CHECK_EQ(neon_csi_release_frame(&e.csi, 0U), NEON_OK);
    CHECK_EQ(neon_csi_release_frame(&e.csi, 0U), NEON_INVALID_PARAM);

    in.timestamp_ns = 2000U;
    CHECK_EQ(fx1_csi_sim_push_frame(&e.csi, &in), NEON_OK);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_OK);
    CHECK_EQ(frame.buffer_index, 1U); /* Ping-pong. */
    CHECK_EQ(frame.base_addr, BUF1);
    CHECK_EQ(frame.frame_id, 1U);

    CHECK_EQ(neon_csi_get_status(&e.csi, &status), NEON_OK);
    CHECK_EQ(status.state, NEON_CSI_STATE_RUNNING);
    CHECK_EQ(status.frame_count, 2U);
    CHECK_EQ(status.buffer_state[0], NEON_CSI_BUFFER_HW_OWNED);
    CHECK_EQ(status.buffer_state[1], NEON_CSI_BUFFER_SW_VALID);
    CHECK_EQ(status.core_state, 1U);
}

static void test_oldest_first_across_wrap(void)
{
    env_t e;
    neon_csi_frame_t frame;

    env_running(&e, true, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    fx1_csi_sim_set_next_frame_id(&e.csi, 0x3FFFU);
    push(&e, 1U); /* Buffer 0, id 0x3FFF. */
    push(&e, 2U); /* Buffer 1, id 0: the 14-bit counter wrapped. */
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_OK);
    CHECK_EQ(frame.buffer_index, 0U);
    CHECK_EQ(frame.frame_id, 0x3FFFU);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_OK);
    CHECK_EQ(frame.buffer_index, 1U);
    CHECK_EQ(frame.frame_id, 0U);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_BUSY);
    CHECK_EQ(neon_csi_release_frame(&e.csi, 0U), NEON_OK);
    CHECK_EQ(neon_csi_release_frame(&e.csi, 1U), NEON_OK);

    /* The older frame is in buffer 1 when buffer 0 was recycled first. */
    CHECK_EQ(neon_csi_stop(&e.csi), NEON_OK);
    CHECK_EQ(neon_csi_start(&e.csi), NEON_OK);
    fx1_csi_sim_set_next_frame_id(&e.csi, 0x3FFEU);
    push(&e, 1U); /* A: buffer 0, id 0x3FFE. */
    push(&e, 2U); /* B: buffer 1, id 0x3FFF. */
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_OK);
    CHECK_EQ(frame.frame_id, 0x3FFEU);
    CHECK_EQ(neon_csi_release_frame(&e.csi, frame.buffer_index), NEON_OK);
    push(&e, 3U); /* C: buffer 0 again, id 0. */
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_OK);
    CHECK_EQ(frame.buffer_index, 1U);
    CHECK_EQ(frame.frame_id, 0x3FFFU);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_OK);
    CHECK_EQ(frame.buffer_index, 0U);
    CHECK_EQ(frame.frame_id, 0U);
}

static void test_order_without_metadata(void)
{
    env_t e;
    neon_csi_frame_t frame;

    /* No per-buffer frame id: the HAL follows the ping-pong alternation and says that the context is unknown. */
    env_running(&e, false, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    push(&e, 1U); /* Buffer 0. */
    push(&e, 2U); /* Buffer 1. */
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_OK);
    CHECK_EQ(frame.buffer_index, 0U);
    CHECK(!frame.context_valid);
    CHECK_EQ(frame.timestamp_ns, 0U);
    CHECK_EQ(neon_csi_release_frame(&e.csi, 0U), NEON_OK);
    push(&e, 3U); /* Buffer 0 again; buffer 1 is the older one. */
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_OK);
    CHECK_EQ(frame.buffer_index, 1U);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_OK);
    CHECK_EQ(frame.buffer_index, 0U);
    CHECK(frame.context_valid); /* The newest capture is the one the registers describe. */
    CHECK_EQ(frame.timestamp_ns, 3U);
}

static void test_drop_when_both_held(void)
{
    env_t e;
    neon_csi_frame_t frame;
    fx1_csi_sim_frame_t in = frame_at(0U);
    neon_csi_status_t status;
    uint32_t events;

    env_running(&e, true, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    push(&e, 1U); /* id 0 */
    push(&e, 2U); /* id 1 */
    CHECK_EQ(fx1_csi_sim_push_frame(&e.csi, &in), NEON_BUSY); /* id 2 is dropped. */
    (void)neon_csi_get_status(&e.csi, &status);
    CHECK_EQ(status.frame_drop_count, 1U);
    (void)neon_csi_get_events(&e.csi, &events);
    CHECK((events & NEON_CSI_EVENT_FRAME_DROPPED) != 0UL);
    CHECK(status.irq_pending);

    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_OK);
    CHECK_EQ(frame.frame_id, 0U);
    CHECK_EQ(neon_csi_release_frame(&e.csi, frame.buffer_index), NEON_OK);
    push(&e, 4U); /* id 3 */
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_OK);
    CHECK_EQ(frame.frame_id, 1U);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_OK);
    CHECK_EQ(frame.frame_id, 3U); /* The gap shows the dropped frame. */
}

static void test_error_policies(void)
{
    env_t e;
    neon_csi_frame_t frame;
    fx1_csi_sim_frame_t bad = frame_at(500U);
    neon_csi_status_t status;
    uint32_t events;

    bad.fault = true;
    bad.last_valid_line = 10U;

    env_running(&e, false, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    CHECK_EQ(fx1_csi_sim_push_frame(&e.csi, &bad), NEON_OK);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_OK);
    CHECK_EQ(frame.state, NEON_CSI_BUFFER_SW_ERROR);
    CHECK_EQ(frame.last_valid_line, 10U);
    CHECK_EQ(frame.vc[0].line_count, 10U);
    CHECK_EQ(frame.vc[0].err_summary, 1U);
    (void)neon_csi_get_events(&e.csi, &events);
    CHECK((events & NEON_CSI_EVENT_BUFFER_ERROR) != 0UL);
    CHECK_EQ(neon_csi_release_frame(&e.csi, frame.buffer_index), NEON_OK);
    (void)neon_csi_stop(&e.csi);

    env_running(&e, false, NEON_CSI_ERROR_POLICY_DISCARD_AND_REWIND);
    CHECK_EQ(fx1_csi_sim_push_frame(&e.csi, &bad), NEON_OK);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_BUSY); /* Nothing reached software. */
    (void)neon_csi_get_status(&e.csi, &status);
    CHECK_EQ(status.frame_drop_count, 1U);
    CHECK_EQ(status.buffer_state[0], NEON_CSI_BUFFER_HW_OWNED);
    push(&e, 600U);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_OK);
    CHECK_EQ(frame.buffer_index, 0U); /* The rewound side is written again. */
    CHECK_EQ(frame.state, NEON_CSI_BUFFER_SW_VALID);
    CHECK_EQ(frame.frame_id, 1U); /* The discarded frame consumed id 0. */
}

static void test_context_validity(void)
{
    env_t e;
    neon_csi_frame_t first;
    neon_csi_frame_t second;

    /* Without the metadata record the registers describe only the last completed capture. */
    env_running(&e, false, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    push(&e, 100U);
    push(&e, 200U);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &first), NEON_OK);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &second), NEON_OK);
    CHECK_EQ(first.buffer_index, 0U);
    CHECK(!first.context_valid); /* A newer capture has completed. */
    CHECK_EQ(first.timestamp_ns, 0U);
    CHECK(second.context_valid);
    CHECK_EQ(second.frame_id, 1U);
    CHECK_EQ(second.timestamp_ns, 200U);
    (void)neon_csi_stop(&e.csi);

    env_running(&e, true, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    push(&e, 100U);
    push(&e, 200U);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &first), NEON_OK);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &second), NEON_OK);
    CHECK(first.context_valid);
    CHECK(second.context_valid);
    CHECK_EQ(first.timestamp_ns, 100U);
    CHECK_EQ(first.frame_id, 0U);
    CHECK_EQ(second.timestamp_ns, 200U);
}

static void test_metadata_extraction(void)
{
    env_t e;
    neon_csi_frame_t frame;
    fx1_csi_sim_frame_t in = frame_at(10U);

    in.extracted[0][0] = 0xAABBCCDDUL;
    in.extracted[0][1] = 0x1122U;
    env_running(&e, true, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    CHECK_EQ(e.csi.meta_base_addr[0], META0);
    CHECK_EQ(fx1_csi_sim_push_frame(&e.csi, &in), NEON_OK);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_OK);
    CHECK_EQ(frame.record_id, 0x1234U);
    CHECK_EQ(frame.vc[0].extracted[0], 0xAABBCCDDUL);
    CHECK_EQ(frame.vc[0].extracted[1], 0x1122U);
    CHECK_EQ(neon_csi_release_frame(&e.csi, frame.buffer_index), NEON_OK);

    CHECK_EQ(neon_csi_set_record_id(&e.csi, 0x77U), NEON_OK);
    CHECK_EQ(fx1_csi_sim_get_record_id(&e.csi), 0x77U);
    CHECK_EQ(fx1_csi_sim_push_frame(&e.csi, &in), NEON_OK);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_OK);
    CHECK_EQ(frame.record_id, 0x77U);
    (void)neon_csi_stop(&e.csi);

    env_running(&e, false, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    CHECK_EQ(fx1_csi_sim_push_frame(&e.csi, &in), NEON_OK);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_OK);
    CHECK_EQ(frame.vc[0].extracted[0], 0U); /* No metadata record, no extraction. */
}

static void test_events_and_mask(void)
{
    env_t e;
    neon_csi_status_t status;
    uint32_t events;

    env_running(&e, false, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    CHECK_EQ(fx1_csi_sim_get_event_mask(&e.csi), e.config.event_mask);
    (void)neon_csi_get_status(&e.csi, &status);
    CHECK(!status.irq_pending);
    push(&e, 1U);
    (void)neon_csi_get_status(&e.csi, &status);
    CHECK(status.irq_pending);
    CHECK_EQ(neon_csi_clear_events(&e.csi, NEON_CSI_EVENT_FRAME_RECEIVED), NEON_OK);
    (void)neon_csi_get_events(&e.csi, &events);
    CHECK((events & NEON_CSI_EVENT_FRAME_RECEIVED) == 0UL);
    CHECK((events & NEON_CSI_EVENT_BUFFER_VALID) != 0UL); /* Sticky until cleared, even when masked. */
    (void)neon_csi_get_status(&e.csi, &status);
    CHECK(!status.irq_pending); /* BUFFER_VALID is not in the mask. */
    CHECK_EQ(neon_csi_set_event_mask(&e.csi, NEON_CSI_EVENT_BUFFER_VALID), NEON_OK);
    CHECK_EQ(fx1_csi_sim_get_event_mask(&e.csi), NEON_CSI_EVENT_BUFFER_VALID);
    (void)neon_csi_get_status(&e.csi, &status);
    CHECK(status.irq_pending);
    CHECK_EQ(neon_csi_set_event_mask(&e.csi, 1UL << 30), NEON_INVALID_PARAM);
    CHECK_EQ(neon_csi_clear_events(&e.csi, NEON_CSI_EVENT_BUFFER_VALID), NEON_OK);

    fx1_csi_sim_raise_events(&e.csi, NEON_CSI_EVENT_AXI_ERROR | NEON_CSI_EVENT_LANE_ABORT);
    (void)neon_csi_get_events(&e.csi, &events);
    CHECK_EQ(events, NEON_CSI_EVENT_AXI_ERROR | NEON_CSI_EVENT_LANE_ABORT);
    CHECK_EQ(neon_csi_get_events(&e.csi, NULL), NEON_INVALID_PARAM);
}

static void test_phy_recovery_under_running_receiver(void)
{
    env_t e;
    uint32_t events;
    neon_csi_status_t status;

    env_running(&e, false, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    CHECK_EQ(neon_mipi_recover(&e.link, NEON_MIPI_RECOVER_PHY, POLLS), NEON_OK);
    (void)neon_csi_get_events(&e.csi, &events);
    CHECK((events & NEON_CSI_EVENT_LINK_RESET) != 0UL); /* Software event, bit 31. */
    (void)neon_csi_get_status(&e.csi, &status);
    CHECK_EQ(status.state, NEON_CSI_STATE_RUNNING); /* The CSR configuration survived a PHY reset. */
    CHECK_EQ(neon_csi_clear_events(&e.csi, NEON_CSI_EVENT_LINK_RESET), NEON_OK);
    (void)neon_csi_get_events(&e.csi, &events);
    CHECK((events & NEON_CSI_EVENT_LINK_RESET) == 0UL);
}

static void test_full_reset_loses_configuration(void)
{
    env_t e;
    neon_csi_frame_t frame;
    neon_csi_status_t status;

    env_running(&e, false, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    CHECK_EQ(neon_mipi_recover(&e.link, NEON_MIPI_RECOVER_FULL, POLLS), NEON_OK);
    (void)neon_csi_get_status(&e.csi, &status);
    CHECK_EQ(status.state, NEON_CSI_STATE_CONFIG_LOST);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_ERROR);
    CHECK_EQ(neon_csi_start(&e.csi), NEON_ERROR);
    CHECK_EQ(neon_csi_configure(&e.csi, &e.config), NEON_BUSY); /* Still holds the link lock. */
    CHECK_EQ(neon_csi_stop(&e.csi), NEON_OK);
    CHECK_EQ(e.link.lock_count, 0U);
    CHECK_EQ(neon_csi_configure(&e.csi, &e.config), NEON_OK);
    CHECK_EQ(e.csi.state, NEON_CSI_STATE_CONFIGURED);
    CHECK_EQ(neon_csi_start(&e.csi), NEON_OK);
    push(&e, 1U);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_OK);

    /* A configured but stopped receiver notices the reset as well. */
    CHECK_EQ(neon_csi_stop(&e.csi), NEON_OK);
    CHECK_EQ(neon_mipi_recover(&e.link, NEON_MIPI_RECOVER_FULL, POLLS), NEON_OK);
    CHECK_EQ(neon_csi_start(&e.csi), NEON_ERROR);
    (void)neon_csi_get_status(&e.csi, &status);
    CHECK_EQ(status.state, NEON_CSI_STATE_CONFIG_LOST);
}

static void test_soft_recovery(void)
{
    env_t e;
    neon_csi_frame_t frame;
    neon_csi_status_t status;
    fx1_csi_sim_counters_t counters;

    env_configured(&e, false, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    CHECK_EQ(neon_csi_recover(&e.csi, NEON_CSI_RECOVER_SOFT), NEON_ERROR); /* Not running. */
    CHECK_EQ(neon_csi_start(&e.csi), NEON_OK);
    CHECK_EQ(neon_csi_recover(&e.csi, (neon_csi_recover_level_t)3), NEON_INVALID_PARAM);
    /* soft_reset is synchronised into rxbyteclkhs, which stops with the clock lane in Stop State. */
    CHECK_EQ(neon_csi_recover(&e.csi, NEON_CSI_RECOVER_SOFT), NEON_ERROR);
    fx1_csi_sim_get_counters(&e.csi, &counters);
    CHECK_EQ(counters.soft_resets, 0U);

    fx1_mipi_sim_set_hs_active(&e.link, true);
    push(&e, 1U);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_OK);
    CHECK_EQ(neon_csi_recover(&e.csi, NEON_CSI_RECOVER_SOFT), NEON_OK);
    fx1_csi_sim_get_counters(&e.csi, &counters);
    CHECK_EQ(counters.soft_resets, 1U);
    (void)neon_csi_get_status(&e.csi, &status);
    CHECK_EQ(status.state, NEON_CSI_STATE_RUNNING);
    CHECK_EQ(status.buffer_state[0], NEON_CSI_BUFFER_HW_OWNED);
    CHECK_EQ(status.buffer_state[1], NEON_CSI_BUFFER_HW_OWNED);
    CHECK_EQ(neon_csi_release_frame(&e.csi, frame.buffer_index), NEON_INVALID_PARAM); /* The frame was lost. */
    push(&e, 2U);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_OK);
}

static void test_stop_failure_keeps_the_link(void)
{
    env_t e;

    env_running(&e, false, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    fx1_csi_sim_set_stop_fails(&e.csi, true);
    CHECK_EQ(neon_csi_stop(&e.csi), NEON_TIMEOUT);
    CHECK_EQ(e.csi.state, NEON_CSI_STATE_RUNNING); /* The DMA may still run: do not release the link. */
    CHECK_EQ(e.link.lock_count, 1U);
    fx1_csi_sim_set_stop_fails(&e.csi, false);
    CHECK_EQ(neon_csi_stop(&e.csi), NEON_OK);
    CHECK_EQ(e.link.lock_count, 0U);
}

static void test_stop_drops_held_frames(void)
{
    env_t e;
    neon_csi_frame_t frame;

    env_running(&e, false, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    push(&e, 1U);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_OK);
    CHECK_EQ(neon_csi_stop(&e.csi), NEON_OK);
    CHECK_EQ(neon_csi_start(&e.csi), NEON_OK);
    CHECK_EQ(neon_csi_release_frame(&e.csi, frame.buffer_index), NEON_INVALID_PARAM);
    push(&e, 2U);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_OK);
    CHECK_EQ(frame.buffer_index, 0U);
}

static void test_short_packet(void)
{
    env_t e;
    neon_csi_short_packet_t in = {1U, 0x08U, 0x1234U};
    neon_csi_short_packet_t out;

    env_configured(&e, false, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    CHECK_EQ(neon_csi_read_short_packet(&e.csi, &out), NEON_ERROR); /* Not running. */
    (void)neon_csi_start(&e.csi);
    CHECK_EQ(neon_csi_read_short_packet(&e.csi, &out), NEON_BUSY);
    fx1_csi_sim_push_short_packet(&e.csi, &in);
    CHECK_EQ(neon_csi_read_short_packet(&e.csi, &out), NEON_OK);
    CHECK_EQ(out.virtual_channel, 1U);
    CHECK_EQ(out.data_type, 0x08U);
    CHECK_EQ(out.data, 0x1234U);
    CHECK_EQ(neon_csi_read_short_packet(&e.csi, &out), NEON_BUSY);
    CHECK_EQ(neon_csi_read_short_packet(&e.csi, NULL), NEON_INVALID_PARAM);
}

static void test_test_pattern_geometry(void)
{
    env_t e;
    neon_csi_frame_t frame;

    link_up(&e);
    (void)neon_csi_init(&e.csi, &e.link);
    make_config(&e.config, false, NEON_CSI_ERROR_POLICY_RELEASE_WITH_ERROR);
    e.config.width = 1920U; /* The sensor geometry is ignored while the generator is on. */
    e.config.height = 1080U;
    e.config.test_pattern.enable = true;
    e.config.test_pattern.pixel_format = NEON_CSI_RAW12;
    e.config.test_pattern.width = WIDTH;
    e.config.test_pattern.height = HEIGHT;
    CHECK_EQ(neon_csi_configure(&e.csi, &e.config), NEON_OK);
    CHECK_EQ(e.csi.layout.width, WIDTH);
    CHECK_EQ(neon_csi_start(&e.csi), NEON_OK);
    push(&e, 1U);
    CHECK_EQ(neon_csi_acquire_frame(&e.csi, &frame), NEON_OK);
    CHECK_EQ(frame.layout.height, HEIGHT);
}

int main(void)
{
    RUN(test_fx1_caps);
    RUN(test_core_follows_caps);
    RUN(test_chip_with_one_buffer_and_narrow_frame_id);
    RUN(test_arguments_and_defaults);
    RUN(test_layout);
    RUN(test_configure_validation);
    RUN(test_start_stop_and_link_lock);
    RUN(test_frame_flow);
    RUN(test_oldest_first_across_wrap);
    RUN(test_order_without_metadata);
    RUN(test_drop_when_both_held);
    RUN(test_error_policies);
    RUN(test_context_validity);
    RUN(test_metadata_extraction);
    RUN(test_events_and_mask);
    RUN(test_phy_recovery_under_running_receiver);
    RUN(test_full_reset_loses_configuration);
    RUN(test_soft_recovery);
    RUN(test_stop_failure_keeps_the_link);
    RUN(test_stop_drops_held_frames);
    RUN(test_short_packet);
    RUN(test_test_pattern_geometry);
    return TEST_REPORT();
}
