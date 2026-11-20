#include "neon_camera_runtime.h"

#include <string.h>

/* Everything the geometry and format decide, computed once for both memory_bytes() and init(). */
typedef struct {
    neon_csi_config_t csi_config;   /* Geometry, format, line padding and buffer size; addresses not yet set. */
    neon_csi_layout_t layout;
    uint32_t align;
    uint32_t y_stride;
    uint32_t uv_stride;
    uint32_t y_bytes;               /* Bytes in use. */
    uint32_t uv_bytes;
    uint32_t raw_slot;              /* Bytes reserved in the region, rounded up to align. */
    uint32_t y_slot;
    uint32_t uv_slot;
    uint8_t raw_buffers;
    uint8_t output_buffers;
    uint64_t total_bytes;
    neon_isp_caps_t isp_caps;
} plan_t;

static uint64_t align_up(uint64_t value, uint32_t alignment)
{
    return alignment <= 1U ? value : (value + alignment - 1U) / alignment * alignment;
}

/* Least common multiple of two alignments, 0 meaning none. 0 when it does not fit 32 bits. */
static uint32_t alignment_lcm(uint32_t a, uint32_t b)
{
    uint32_t x = a;
    uint32_t y = b;
    uint64_t lcm;

    if (a == 0U) return b;
    if (b == 0U) return a;
    while (y != 0U) {
        const uint32_t t = x % y;
        x = y;
        y = t;
    }
    lcm = (uint64_t)a / x * b;
    return lcm > UINT32_MAX ? 0U : (uint32_t)lcm;
}

static neon_status_t csi_raw_format(neon_isp_raw_depth_t depth, neon_csi_raw_format_t *format)
{
    switch (depth) {
    case NEON_ISP_RAW10:
        *format = NEON_CSI_RAW10;
        return NEON_OK;
    case NEON_ISP_RAW12:
        *format = NEON_CSI_RAW12;
        return NEON_OK;
    default:
        return NEON_INVALID_PARAM;
    }
}

static neon_status_t make_plan(const neon_camera_runtime_config_t *config, plan_t *plan)
{
    neon_csi_caps_t csi_caps;
    neon_csi_raw_format_t raw_format;
    uint32_t padding;
    neon_status_t status;

    if (config == NULL || plan == NULL) {
        return NEON_INVALID_PARAM;
    }
    (void)memset(plan, 0, sizeof(*plan));
    if (config->width == 0U || config->width > UINT16_MAX || config->height == 0U || config->height > UINT16_MAX) {
        return NEON_INVALID_PARAM;
    }
    status = csi_raw_format(config->raw_depth, &raw_format);
    if (status == NEON_OK) status = neon_csi_get_caps(&csi_caps);
    if (status == NEON_OK) status = neon_isp_get_caps(&plan->isp_caps);
    if (status != NEON_OK) {
        return status;
    }
    if (csi_caps.buffer_count == 0U || csi_caps.buffer_count > NEON_CSI_MAX_BUFFERS ||
        plan->isp_caps.output_buffers == 0U || plan->isp_caps.output_buffers > NEON_ISP_MAX_BUFFERS) {
        return NEON_NOT_SUPPORTED;
    }

    neon_csi_get_default_config(&plan->csi_config);
    plan->csi_config.raw_format = raw_format;
    plan->csi_config.width = (uint16_t)config->width;
    plan->csi_config.height = (uint16_t)config->height;
    status = neon_csi_calculate_layout(&plan->csi_config, &plan->layout);
    if (status != NEON_OK) {
        return status;
    }

    /* One alignment for the pitch and for every buffer that satisfies both DMA engines. */
    plan->align = alignment_lcm(csi_caps.dma_align_bytes, plan->isp_caps.dma_align_bytes);
    if (plan->align == 0U && (csi_caps.dma_align_bytes != 0U || plan->isp_caps.dma_align_bytes != 0U)) {
        return NEON_NOT_SUPPORTED;
    }
    padding = (uint32_t)(align_up(plan->layout.bytes_per_line, plan->align) - plan->layout.bytes_per_line);
    if (padding > UINT16_MAX) {
        return NEON_NOT_SUPPORTED;
    }
    plan->csi_config.dma.line_padding_bytes = (uint16_t)padding;
    status = neon_csi_calculate_layout(&plan->csi_config, &plan->layout);
    if (status != NEON_OK) {
        return status;
    }
    plan->csi_config.dma.buffer_size_bytes = plan->layout.frame_bytes;

    /* The ISP reads 12-bit samples that are MSB aligned. For 12 bits that is the receiver's right-aligned
     * RAW16; for fewer bits only a receiver that left-aligns produces it. */
    if (config->raw_depth != NEON_ISP_RAW12 && plan->layout.sample_align != NEON_CSI_ALIGN_LEFT) {
        return NEON_NOT_SUPPORTED;
    }

    plan->y_stride = (uint32_t)align_up(config->width, plan->align);
    plan->uv_stride = plan->y_stride; /* Interleaved Cb/Cr at half the width: the same bytes per line as Y. */
    plan->y_bytes = plan->y_stride * config->height;
    plan->uv_bytes = plan->uv_stride * ((config->height + 1U) / 2U);
    plan->raw_slot = (uint32_t)align_up(plan->layout.frame_bytes, plan->align);
    plan->y_slot = (uint32_t)align_up(plan->y_bytes, plan->align);
    plan->uv_slot = (uint32_t)align_up(plan->uv_bytes, plan->align);
    plan->raw_buffers = csi_caps.buffer_count;
    plan->output_buffers = plan->isp_caps.output_buffers;
    plan->total_bytes = (uint64_t)plan->raw_buffers * plan->raw_slot +
                        (uint64_t)plan->output_buffers * ((uint64_t)plan->y_slot + plan->uv_slot);
    return plan->total_bytes > UINT32_MAX ? NEON_NOT_SUPPORTED : NEON_OK;
}

void neon_camera_runtime_get_default_config(neon_camera_runtime_config_t *config)
{
    if (config == NULL) {
        return;
    }
    (void)memset(config, 0, sizeof(*config));
    config->raw_depth = NEON_ISP_RAW12;
    config->bayer_pattern = NEON_ISP_BAYER_RGGB;
    config->continuous_clock = true;
    config->mipi_ready_timeout_polls = NEON_CAMERA_RUNTIME_DEFAULT_READY_POLLS;
    config->algorithm_ops = neon_camera_3a_reference_ops();
}

neon_status_t neon_camera_runtime_memory_bytes(const neon_camera_runtime_config_t *config, uint32_t *bytes)
{
    plan_t plan;
    neon_status_t status;

    if (bytes == NULL) {
        return NEON_INVALID_PARAM;
    }
    status = make_plan(config, &plan);
    if (status == NEON_OK) {
        *bytes = (uint32_t)plan.total_bytes;
    }
    return status;
}

static void deinit_handles(neon_camera_runtime_t *runtime)
{
    (void)neon_isp_deinit(&runtime->isp);
    (void)neon_csi_deinit(&runtime->csi);
    (void)neon_mipi_deinit(&runtime->mipi);
    (void)memset(runtime, 0, sizeof(*runtime));
}

/* A tuning binary is either absent (NULL, 0) or present (pointer and size). */
static bool blob_consistent(const void *data, uint32_t bytes)
{
    return (data == NULL) == (bytes == 0U);
}

neon_status_t neon_camera_runtime_init(neon_camera_runtime_t *runtime, const neon_camera_runtime_config_t *config)
{
    neon_isp_profile_info_t profile_info;
    const neon_camera_3a_ops_t *algorithm_ops;
    plan_t plan;
    neon_camera_pipeline_t *pipeline;
    neon_isp_config_t *isp_config;
    uint64_t address;
    uint64_t region_end;
    uint8_t i;
    neon_status_t status;

    if (runtime == NULL || config == NULL || config->sensor == NULL || config->sensor_ops == NULL ||
        config->lane_count == 0U || config->lane_rate_bps == 0U) {
        return NEON_INVALID_PARAM;
    }
    if (!blob_consistent(config->isp_profile, config->isp_profile_bytes) ||
        !blob_consistent(config->algorithm_tuning, config->algorithm_tuning_bytes) ||
        (config->algorithm_tuning != NULL && config->profile != NULL)) {
        return NEON_INVALID_PARAM;
    }
    algorithm_ops = config->algorithm_ops != NULL ? config->algorithm_ops : neon_camera_3a_reference_ops();
    if (config->algorithm_tuning != NULL && algorithm_ops->load_profile == NULL) {
        return NEON_NOT_SUPPORTED;
    }
    status = make_plan(config, &plan);
    if (status != NEON_OK) {
        return status;
    }
    (void)memset(&profile_info, 0, sizeof(profile_info));
    if (config->isp_profile != NULL) {
        status = neon_isp_check_profile(config->isp_profile, config->isp_profile_bytes, &profile_info);
        if (status != NEON_OK) {
            return status;
        }
    }
    /* Addresses are 32 bit, and the first buffer may need to move up to the alignment. */
    address = align_up(config->memory_base, plan.align);
    region_end = (uint64_t)config->memory_base + config->memory_bytes;
    if (address + plan.total_bytes > region_end || region_end > ((uint64_t)UINT32_MAX + 1U)) {
        return NEON_INVALID_PARAM;
    }

    (void)memset(runtime, 0, sizeof(*runtime));
    status = neon_mipi_init(&runtime->mipi);
    if (status == NEON_OK) status = neon_csi_init(&runtime->csi, &runtime->mipi);
    if (status == NEON_OK) status = neon_isp_init(&runtime->isp);
    if (status != NEON_OK) {
        deinit_handles(runtime);
        return status;
    }
    neon_camera_3a_init(&runtime->algorithm, config->profile);
    if (config->algorithm_tuning != NULL &&
        algorithm_ops->load_profile(&runtime->algorithm, config->algorithm_tuning, config->algorithm_tuning_bytes) != 0) {
        deinit_handles(runtime);
        return NEON_INVALID_PARAM;
    }
    runtime->isp_profile_info = profile_info;

    pipeline = &runtime->pipeline;
    pipeline->sensor = config->sensor;
    pipeline->sensor_ops = config->sensor_ops;
    pipeline->sensor_mode_id = config->sensor_mode_id;
    pipeline->mipi = &runtime->mipi;
    pipeline->mipi_config.direction = NEON_MIPI_DIRECTION_RX;
    pipeline->mipi_config.lane_count = config->lane_count;
    pipeline->mipi_config.lane_rate_bps = config->lane_rate_bps;
    pipeline->mipi_config.continuous_clock = config->continuous_clock;
    pipeline->mipi_ready_timeout_polls = config->mipi_ready_timeout_polls != 0U ?
        config->mipi_ready_timeout_polls : NEON_CAMERA_RUNTIME_DEFAULT_READY_POLLS;
    pipeline->csi = &runtime->csi;
    pipeline->csi_config = plan.csi_config;
    pipeline->isp = &runtime->isp;
    pipeline->algorithm = &runtime->algorithm;
    pipeline->algorithm_ops = algorithm_ops;
    pipeline->isp_profile = config->isp_profile;
    pipeline->isp_profile_bytes = config->isp_profile_bytes;

    isp_config = &pipeline->isp_config;
    neon_isp_get_default_config(isp_config);
    isp_config->width = (uint16_t)config->width;
    isp_config->height = (uint16_t)config->height;
    isp_config->raw_depth = config->raw_depth;
    isp_config->bayer_pattern = config->bayer_pattern;
    isp_config->input_stride_bytes = plan.layout.line_pitch_bytes;
    isp_config->y_stride_bytes = plan.y_stride;
    isp_config->uv_stride_bytes = plan.uv_stride;
    isp_config->output_y_bytes = plan.y_bytes;
    isp_config->output_uv_bytes = plan.uv_bytes;
    isp_config->stats_aec = plan.isp_caps.aec;
    isp_config->stats_awb = plan.isp_caps.awb;
    isp_config->stats_af = plan.isp_caps.af;

    for (i = 0U; i < plan.raw_buffers; i++) {
        pipeline->csi_config.dma.base_addr[i] = (uint32_t)address;
        address += plan.raw_slot;
    }
    for (i = 0U; i < plan.output_buffers; i++) {
        isp_config->output_y_addr[i] = (uint32_t)address;
        address += plan.y_slot;
        isp_config->output_uv_addr[i] = (uint32_t)address;
        address += plan.uv_slot;
    }
    return NEON_OK;
}

neon_status_t neon_camera_runtime_deinit(neon_camera_runtime_t *runtime)
{
    if (runtime == NULL) {
        return NEON_INVALID_PARAM;
    }
    /* Look before touching anything: a deinit of one handle wipes it, and a half-done teardown would leave a
     * running stage with no handle. */
    if (runtime->isp.state == NEON_ISP_STATE_RUNNING || runtime->csi.state == NEON_CSI_STATE_RUNNING ||
        runtime->mipi.state != NEON_MIPI_STATE_RELEASED) {
        return NEON_BUSY;
    }
    deinit_handles(runtime);
    return NEON_OK;
}
