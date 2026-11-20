/**
 * @file neon_isp.c
 * @brief ISP: validation, state machine, buffer rotation and ownership rules, independent of the backend.
 */

/*==================================================================================================
*                                          INCLUDE FILES                                          
==================================================================================================*/
#include "isp/neon_isp.h"
#include "isp/platform/common/neon_isp_hw.h"
#include <stddef.h>
#include <string.h>

/*==================================================================================================
*                                        DEFINES AND MACROS                                       
==================================================================================================*/
/* Portable starting values. configure() checks them against the caps of the chip. */
#define ISP_DEFAULT_BURST_BEATS 16U
#define ISP_DEFAULT_EVENT_MASK (NEON_ISP_EVENT_FRAME_DONE | NEON_ISP_EVENT_PIPELINE_ERROR | \
                                NEON_ISP_EVENT_STATS_AEC | NEON_ISP_EVENT_STATS_AWB | NEON_ISP_EVENT_STATS_AF | \
                                NEON_ISP_EVENT_IDMA_DONE | NEON_ISP_EVENT_ODMA_DONE | NEON_ISP_EVENT_ODMA_OVERFLOW | \
                                NEON_ISP_EVENT_AXI_READ_ERROR | NEON_ISP_EVENT_AXI_WRITE_ERROR | \
                                NEON_ISP_EVENT_CONFIG_ERROR)
/* The statistics sums are of 12-bit Bayer samples; the 8-bit means drop the four low bits. */
#define ISP_MEAN_SHIFT 4U
#define ISP_STATS_AEC 0U
#define ISP_STATS_AWB 1U
#define ISP_STATS_AF 2U

/*==================================================================================================
*                                         LOCAL FUNCTIONS                                         
==================================================================================================*/
static bool isp_aligned(const neon_isp_caps_t *caps, uint32_t value)
{
    return caps->dma_align_bytes == 0U || (value % caps->dma_align_bytes) == 0U;
}

static bool isp_overlap(uint32_t a_base, uint64_t a_size, uint32_t b_base, uint64_t b_size)
{
    return (uint64_t)a_base < (uint64_t)b_base + b_size && (uint64_t)b_base < (uint64_t)a_base + a_size;
}

static bool isp_range_bad(const neon_isp_caps_t *caps, uint32_t base, uint64_t size)
{
    return base == 0U || !isp_aligned(caps, base) || size == 0U ||
           (uint64_t)base + size > (uint64_t)UINT32_MAX + 1U;
}

static bool isp_configured(const neon_isp_handle_t *handle)
{
    return handle->state == NEON_ISP_STATE_CONFIGURED || handle->state == NEON_ISP_STATE_RUNNING;
}

static uint8_t isp_slot_mask(uint8_t slot)
{
    return (uint8_t)(1U << slot);
}

static uint8_t isp_mean8(uint64_t sum, uint64_t count)
{
    uint64_t mean;

    if (count == 0U) {
        return 0U;
    }
    mean = (sum / count) >> ISP_MEAN_SHIFT;
    return (mean > 255U) ? 255U : (uint8_t)mean;
}

static bool isp_caps_usable(const neon_isp_caps_t *caps)
{
    return caps->input_buffers >= 1U && caps->input_buffers <= NEON_ISP_MAX_BUFFERS &&
           caps->output_buffers >= 1U && caps->output_buffers <= NEON_ISP_MAX_BUFFERS &&
           caps->aec_hist_bins <= NEON_ISP_MAX_HIST_BINS && (!caps->aec || caps->aec_hist_bins >= 1U) &&
           caps->size_granularity >= 1U && caps->min_width >= 1U && caps->min_width <= caps->max_width &&
           caps->min_height >= 1U && caps->min_height <= caps->max_height && caps->max_burst_beats >= 1U;
}

static neon_status_t isp_validate_config(const neon_isp_caps_t *caps, const neon_isp_config_t *config)
{
    const uint64_t y_needed = (uint64_t)config->y_stride_bytes * config->height;
    const uint64_t uv_needed = (uint64_t)config->uv_stride_bytes * (config->height / 2U);
    uint8_t i;
    uint8_t j;

    if (config->width < caps->min_width || config->width > caps->max_width ||
        config->height < caps->min_height || config->height > caps->max_height ||
        (config->width % caps->size_granularity) != 0U || (config->height % caps->size_granularity) != 0U) {
        return NEON_INVALID_PARAM;
    }
    if (config->raw_depth != NEON_ISP_RAW10 && config->raw_depth != NEON_ISP_RAW12) {
        return NEON_INVALID_PARAM;
    }
    if (((caps->raw_depth_mask >> (unsigned)config->raw_depth) & 1U) == 0U) {
        return NEON_NOT_SUPPORTED;
    }
    if ((unsigned)config->bayer_pattern > (unsigned)NEON_ISP_BAYER_BGGR) {
        return NEON_INVALID_PARAM;
    }
    if (((caps->bayer_pattern_mask >> (unsigned)config->bayer_pattern) & 1U) == 0U) {
        return NEON_NOT_SUPPORTED;
    }
    if ((config->stats_aec && !caps->aec) || (config->stats_awb && !caps->awb) || (config->stats_af && !caps->af)) {
        return NEON_NOT_SUPPORTED;
    }
    if (config->max_burst_beats < 1U || config->max_burst_beats > caps->max_burst_beats ||
        (config->event_mask & ~caps->supported_events) != 0UL) {
        return NEON_INVALID_PARAM;
    }
    if ((uint64_t)config->input_stride_bytes < 2U * (uint64_t)config->width ||
        config->y_stride_bytes < config->width || config->uv_stride_bytes < config->width ||
        !isp_aligned(caps, config->input_stride_bytes) || !isp_aligned(caps, config->y_stride_bytes) ||
        !isp_aligned(caps, config->uv_stride_bytes)) {
        return NEON_INVALID_PARAM;
    }
    if ((uint64_t)config->output_y_bytes < y_needed || (uint64_t)config->output_uv_bytes < uv_needed) {
        return NEON_INVALID_PARAM;
    }
    for (i = 0U; i < caps->output_buffers; i++) {
        if (isp_range_bad(caps, config->output_y_addr[i], config->output_y_bytes) ||
            isp_range_bad(caps, config->output_uv_addr[i], config->output_uv_bytes)) {
            return NEON_INVALID_PARAM;
        }
    }
    /* Every plane of every rotation slot is a separate area; the DMA would otherwise overwrite a frame. */
    for (i = 0U; i < caps->output_buffers; i++) {
        if (isp_overlap(config->output_y_addr[i], config->output_y_bytes, config->output_uv_addr[i], config->output_uv_bytes)) {
            return NEON_INVALID_PARAM;
        }
        for (j = (uint8_t)(i + 1U); j < caps->output_buffers; j++) {
            if (isp_overlap(config->output_y_addr[i], config->output_y_bytes, config->output_y_addr[j], config->output_y_bytes) ||
                isp_overlap(config->output_uv_addr[i], config->output_uv_bytes, config->output_uv_addr[j], config->output_uv_bytes) ||
                isp_overlap(config->output_y_addr[i], config->output_y_bytes, config->output_uv_addr[j], config->output_uv_bytes) ||
                isp_overlap(config->output_uv_addr[i], config->output_uv_bytes, config->output_y_addr[j], config->output_y_bytes)) {
                return NEON_INVALID_PARAM;
            }
        }
    }
    return NEON_OK;
}

/* Inputs that the DMA has not finished with are lost by a stop or a reset; the rest stay what they were. */
static void isp_discard_inputs(neon_isp_handle_t *handle, uint8_t valid_mask)
{
    uint8_t i;

    for (i = 0U; i < handle->input_count; i++) {
        const uint8_t slot = (uint8_t)((handle->input_oldest + i) % handle->caps.input_buffers);

        if ((valid_mask & isp_slot_mask(slot)) != 0U) {
            handle->input_discard_mask |= isp_slot_mask(slot);
        }
    }
    handle->output_held_mask = 0U;
    handle->output_next = 0U;
    (void)memset(handle->stats_frame_id, 0, sizeof(handle->stats_frame_id));
    if (handle->input_count == 0U) {
        handle->input_head = 0U;
        handle->input_oldest = 0U;
        handle->input_flushing = false;
    } else {
        handle->input_flushing = true; /* The hardware rotation restarts at slot 0: wait for the books to empty. */
    }
}

/*==================================================================================================
*                                         GLOBAL FUNCTIONS                                        
==================================================================================================*/
neon_status_t neon_isp_init(neon_isp_handle_t *handle)
{
    if (handle == NULL) {
        return NEON_INVALID_PARAM;
    }
    (void)memset(handle, 0, sizeof(*handle));
    handle->state = NEON_ISP_STATE_IDLE;
    if (neon_isp_hw_get_caps(&handle->caps) != NEON_OK || !isp_caps_usable(&handle->caps)) {
        return NEON_ERROR;
    }
    return neon_isp_hw_init(handle);
}

neon_status_t neon_isp_get_caps(neon_isp_caps_t *caps)
{
    if (caps == NULL) {
        return NEON_INVALID_PARAM;
    }
    (void)memset(caps, 0, sizeof(*caps));
    return neon_isp_hw_get_caps(caps);
}

neon_status_t neon_isp_deinit(neon_isp_handle_t *handle)
{
    if (handle == NULL) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state == NEON_ISP_STATE_RUNNING) {
        return NEON_BUSY;
    }
    (void)memset(handle, 0, sizeof(*handle));
    return NEON_OK;
}

void neon_isp_get_default_config(neon_isp_config_t *config)
{
    if (config == NULL) {
        return;
    }
    (void)memset(config, 0, sizeof(*config));
    config->raw_depth = NEON_ISP_RAW12;
    config->bayer_pattern = NEON_ISP_BAYER_RGGB;
    config->max_burst_beats = ISP_DEFAULT_BURST_BEATS;
    config->stats_aec = true;
    config->stats_awb = true;
    config->stats_af = true;
    config->event_mask = ISP_DEFAULT_EVENT_MASK;
}

neon_status_t neon_isp_configure(neon_isp_handle_t *handle, const neon_isp_config_t *config)
{
    neon_status_t status;
    uint8_t i;

    if (handle == NULL || config == NULL) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state == NEON_ISP_STATE_RUNNING) {
        return NEON_BUSY;
    }
    status = isp_validate_config(&handle->caps, config);
    if (status != NEON_OK) {
        return status;
    }
    status = neon_isp_hw_configure(handle, config);
    if (status != NEON_OK) {
        return status;
    }
    handle->width = config->width;
    handle->height = config->height;
    handle->input_stride_bytes = config->input_stride_bytes;
    handle->y_stride_bytes = config->y_stride_bytes;
    handle->uv_stride_bytes = config->uv_stride_bytes;
    for (i = 0U; i < handle->caps.output_buffers; i++) {
        handle->output_y_addr[i] = config->output_y_addr[i];
        handle->output_uv_addr[i] = config->output_uv_addr[i];
    }
    (void)memset(handle->input, 0, sizeof(handle->input));
    handle->input_head = 0U;
    handle->input_oldest = 0U;
    handle->input_count = 0U;
    handle->input_discard_mask = 0U;
    handle->input_flushing = false;
    handle->output_held_mask = 0U;
    handle->output_next = 0U;
    handle->event_mask = config->event_mask;
    (void)memset(handle->stats_frame_id, 0, sizeof(handle->stats_frame_id));
    handle->state = NEON_ISP_STATE_CONFIGURED;
    return NEON_OK;
}

neon_status_t neon_isp_start(neon_isp_handle_t *handle)
{
    neon_status_t status;

    if (handle == NULL) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state == NEON_ISP_STATE_RUNNING) {
        return NEON_BUSY;
    }
    if (handle->state != NEON_ISP_STATE_CONFIGURED) {
        return NEON_ERROR;
    }
    status = neon_isp_hw_start(handle);
    if (status != NEON_OK) {
        return status;
    }
    handle->output_held_mask = 0U;
    handle->output_next = 0U;
    (void)memset(handle->stats_frame_id, 0, sizeof(handle->stats_frame_id));
    handle->state = NEON_ISP_STATE_RUNNING;
    return NEON_OK;
}

neon_status_t neon_isp_stop(neon_isp_handle_t *handle)
{
    uint8_t valid_mask = 0U;
    uint8_t done_mask = 0U;
    neon_status_t status;

    if (handle == NULL) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state != NEON_ISP_STATE_RUNNING) {
        return NEON_ERROR;
    }
    if (neon_isp_hw_read_buffers(handle, &valid_mask, &done_mask) != NEON_OK) {
        valid_mask = 0xFFU; /* Unknown: report every queued frame as thrown away. */
    }
    status = neon_isp_hw_stop(handle);
    if (status != NEON_OK) {
        return status; /* The DMA may still run: stay running and let the caller retry or recover. */
    }
    isp_discard_inputs(handle, valid_mask);
    handle->state = NEON_ISP_STATE_CONFIGURED;
    return NEON_OK;
}

neon_status_t neon_isp_recover(neon_isp_handle_t *handle, neon_isp_recover_level_t level)
{
    uint8_t valid_mask = 0U;
    uint8_t done_mask = 0U;
    neon_status_t status;

    if (handle == NULL || level != NEON_ISP_RECOVER_SOFT) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state != NEON_ISP_STATE_RUNNING) {
        return NEON_ERROR;
    }
    if (neon_isp_hw_read_buffers(handle, &valid_mask, &done_mask) != NEON_OK) {
        valid_mask = 0xFFU;
    }
    status = neon_isp_hw_soft_reset(handle);
    if (status == NEON_OK) {
        isp_discard_inputs(handle, valid_mask);
    }
    return status;
}

/* Entries are read byte by byte: a profile in flash or in a file buffer has no alignment guarantee. */
static uint32_t isp_read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static neon_status_t isp_scan_profile(const void *profile, uint32_t size_bytes, neon_isp_profile_info_t *info)
{
    const uint8_t *entry = profile;
    uint32_t i;

    (void)memset(info, 0, sizeof(*info));
    if (profile == NULL || size_bytes == 0U || (size_bytes % NEON_ISP_PROFILE_ENTRY_BYTES) != 0U) {
        return NEON_INVALID_PARAM;
    }
    info->entries = size_bytes / NEON_ISP_PROFILE_ENTRY_BYTES;
    for (i = 0U; i < info->entries; i++, entry += NEON_ISP_PROFILE_ENTRY_BYTES) {
        switch (neon_isp_hw_profile_classify(isp_read_le32(entry))) {
        case NEON_ISP_HW_PROFILE_WRITE:
            info->written++;
            break;
        case NEON_ISP_HW_PROFILE_SKIP:
            info->skipped++;
            break;
        default:
            return NEON_INVALID_PARAM;
        }
    }
    return NEON_OK;
}

neon_status_t neon_isp_check_profile(const void *profile, uint32_t size_bytes, neon_isp_profile_info_t *info)
{
    neon_isp_profile_info_t counts;
    neon_status_t status = isp_scan_profile(profile, size_bytes, &counts);

    if (info != NULL) {
        *info = counts;
    }
    return status;
}

neon_status_t neon_isp_load_profile(neon_isp_handle_t *handle, const void *profile, uint32_t size_bytes,
                                    neon_isp_profile_info_t *info)
{
    neon_isp_profile_info_t counts;
    const uint8_t *entry = profile;
    neon_status_t status;
    uint32_t offset;
    uint32_t i;

    if (handle == NULL) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state == NEON_ISP_STATE_RUNNING) {
        return NEON_BUSY;
    }
    status = isp_scan_profile(profile, size_bytes, &counts);
    if (info != NULL) {
        *info = counts;
    }
    if (status != NEON_OK) {
        return status; /* Refused as a whole: nothing was written and the handle is unchanged. */
    }
    /* From the first write on, the configuration is no longer what configure() programmed. */
    handle->state = NEON_ISP_STATE_IDLE;
    for (i = 0U; i < counts.entries; i++, entry += NEON_ISP_PROFILE_ENTRY_BYTES) {
        offset = isp_read_le32(entry);
        if (neon_isp_hw_profile_classify(offset) != NEON_ISP_HW_PROFILE_WRITE) {
            continue;
        }
        status = neon_isp_hw_profile_write(handle, offset, isp_read_le32(entry + 4));
        if (status != NEON_OK) {
            return status;
        }
    }
    return NEON_OK;
}

neon_status_t neon_isp_get_status(neon_isp_handle_t *handle, neon_isp_status_t *status)
{
    uint8_t valid_mask = 0U;
    uint8_t done_mask = 0U;
    uint32_t events;
    neon_status_t result;

    if (handle == NULL || status == NULL) {
        return NEON_INVALID_PARAM;
    }
    (void)memset(status, 0, sizeof(*status));
    status->state = handle->state;
    if (!isp_configured(handle)) {
        return NEON_OK;
    }
    result = neon_isp_hw_read_buffers(handle, &valid_mask, &done_mask);
    if (result == NEON_OK) {
        status->input_queued_mask = valid_mask;
        status->output_held_mask = handle->output_held_mask;
        status->output_done_mask = (uint8_t)(done_mask & ~handle->output_held_mask);
        result = neon_isp_hw_read_counters(handle, status);
    }
    if (result == NEON_OK) {
        result = neon_isp_hw_read_events(handle, &events);
    }
    if (result == NEON_OK) {
        status->irq_pending = (events & handle->event_mask) != 0UL;
    }
    return result;
}

neon_status_t neon_isp_get_events(neon_isp_handle_t *handle, uint32_t *events)
{
    if (handle == NULL || events == NULL) {
        return NEON_INVALID_PARAM;
    }
    if (!isp_configured(handle)) {
        *events = 0UL;
        return NEON_OK;
    }
    return neon_isp_hw_read_events(handle, events);
}

neon_status_t neon_isp_clear_events(neon_isp_handle_t *handle, uint32_t events)
{
    if (handle == NULL) {
        return NEON_INVALID_PARAM;
    }
    if ((events & handle->caps.supported_events) == 0UL || !isp_configured(handle)) {
        return NEON_OK;
    }
    return neon_isp_hw_clear_events(handle, events & handle->caps.supported_events);
}

neon_status_t neon_isp_set_event_mask(neon_isp_handle_t *handle, uint32_t event_mask)
{
    neon_status_t status;

    if (handle == NULL || (event_mask & ~handle->caps.supported_events) != 0UL) {
        return NEON_INVALID_PARAM;
    }
    if (!isp_configured(handle)) {
        return NEON_ERROR;
    }
    status = neon_isp_hw_set_event_mask(handle, event_mask);
    if (status == NEON_OK) {
        handle->event_mask = event_mask;
    }
    return status;
}

neon_status_t neon_isp_queue_input(neon_isp_handle_t *handle, const neon_isp_input_t *input)
{
    neon_status_t status;
    uint8_t slot;

    if (handle == NULL || input == NULL) {
        return NEON_INVALID_PARAM;
    }
    if (!isp_configured(handle)) {
        return NEON_ERROR;
    }
    if (handle->input_flushing || handle->input_count >= handle->caps.input_buffers) {
        return NEON_BUSY;
    }
    if (isp_range_bad(&handle->caps, input->address, input->size_bytes) ||
        (uint64_t)input->size_bytes < (uint64_t)handle->input_stride_bytes * handle->height) {
        return NEON_INVALID_PARAM;
    }
    slot = handle->input_head;
    status = neon_isp_hw_queue_input(handle, slot, input);
    if (status != NEON_OK) {
        return status;
    }
    handle->input[slot] = *input;
    handle->input[slot].processed = false;
    handle->input_discard_mask &= (uint8_t)~isp_slot_mask(slot);
    handle->input_head = (uint8_t)((slot + 1U) % handle->caps.input_buffers);
    handle->input_count++;
    return NEON_OK;
}

neon_status_t neon_isp_reclaim_input(neon_isp_handle_t *handle, neon_isp_input_t *input)
{
    uint8_t valid_mask = 0U;
    uint8_t done_mask = 0U;
    uint8_t slot;
    bool discarded;
    neon_status_t status;

    if (handle == NULL || input == NULL) {
        return NEON_INVALID_PARAM;
    }
    if (!isp_configured(handle)) {
        return NEON_ERROR;
    }
    if (handle->input_count == 0U) {
        return NEON_BUSY;
    }
    slot = handle->input_oldest;
    discarded = (handle->input_discard_mask & isp_slot_mask(slot)) != 0U;
    if (!discarded) {
        status = neon_isp_hw_read_buffers(handle, &valid_mask, &done_mask);
        if (status != NEON_OK) {
            return status;
        }
        if ((valid_mask & isp_slot_mask(slot)) != 0U) {
            return NEON_BUSY; /* Still waiting for, or being read by, the input DMA. */
        }
    }
    *input = handle->input[slot];
    input->processed = !discarded;
    handle->input_discard_mask &= (uint8_t)~isp_slot_mask(slot);
    handle->input_oldest = (uint8_t)((slot + 1U) % handle->caps.input_buffers);
    handle->input_count--;
    if (handle->input_count == 0U && handle->input_flushing) {
        handle->input_head = 0U;
        handle->input_oldest = 0U;
        handle->input_flushing = false;
    }
    return NEON_OK;
}

neon_status_t neon_isp_acquire_output(neon_isp_handle_t *handle, neon_isp_output_t *output)
{
    uint8_t valid_mask = 0U;
    uint8_t done_mask = 0U;
    const uint8_t count = (handle != NULL) ? handle->caps.output_buffers : 0U;
    uint8_t step;
    neon_status_t status;

    if (handle == NULL || output == NULL) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state != NEON_ISP_STATE_RUNNING) {
        return NEON_ERROR;
    }
    status = neon_isp_hw_read_buffers(handle, &valid_mask, &done_mask);
    if (status != NEON_OK) {
        return status;
    }
    for (step = 0U; step < count; step++) {
        const uint8_t slot = (uint8_t)((handle->output_next + step) % count);

        if ((done_mask & isp_slot_mask(slot)) != 0U && (handle->output_held_mask & isp_slot_mask(slot)) == 0U) {
            (void)memset(output, 0, sizeof(*output));
            status = neon_isp_hw_read_output(handle, slot, output);
            if (status != NEON_OK) {
                return status;
            }
            output->buffer_index = slot;
            output->y_addr = handle->output_y_addr[slot];
            output->uv_addr = handle->output_uv_addr[slot];
            output->y_stride_bytes = handle->y_stride_bytes;
            output->uv_stride_bytes = handle->uv_stride_bytes;
            output->width = handle->width;
            output->height = handle->height;
            handle->output_held_mask |= isp_slot_mask(slot);
            handle->output_next = (uint8_t)((slot + 1U) % count);
            return NEON_OK;
        }
    }
    return NEON_BUSY;
}

neon_status_t neon_isp_release_output(neon_isp_handle_t *handle, uint8_t buffer_index)
{
    neon_status_t status;

    if (handle == NULL || buffer_index >= handle->caps.output_buffers) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state != NEON_ISP_STATE_RUNNING) {
        return NEON_ERROR;
    }
    if ((handle->output_held_mask & isp_slot_mask(buffer_index)) == 0U) {
        return NEON_INVALID_PARAM;
    }
    status = neon_isp_hw_release_output(handle, buffer_index);
    if (status == NEON_OK) {
        handle->output_held_mask &= (uint8_t)~isp_slot_mask(buffer_index);
    }
    return status;
}

neon_status_t neon_isp_read_statistics(neon_isp_handle_t *handle, neon_isp_statistics_t *statistics)
{
    neon_isp_statistics_t raw;
    uint32_t seen[3];
    bool fresh = false;
    neon_status_t status;
    uint32_t i;

    if (handle == NULL || statistics == NULL) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state != NEON_ISP_STATE_RUNNING) {
        return NEON_ERROR;
    }
    (void)memset(&raw, 0, sizeof(raw));
    status = neon_isp_hw_read_statistics(handle, &raw);
    if (status != NEON_OK) {
        return status;
    }
    seen[ISP_STATS_AEC] = raw.aec.valid ? raw.aec.frame_id : 0U;
    seen[ISP_STATS_AWB] = raw.awb.valid ? raw.awb.frame_id : 0U;
    seen[ISP_STATS_AF] = raw.af.valid ? raw.af.frame_id : 0U;
    for (i = 0U; i < 3U; i++) {
        if (seen[i] != 0U && seen[i] != handle->stats_frame_id[i]) {
            fresh = true;
        }
    }
    if (!fresh) {
        return NEON_BUSY;
    }
    for (i = 0U; i < 3U; i++) {
        if (seen[i] != 0U) {
            handle->stats_frame_id[i] = seen[i];
        }
    }
    if (raw.aec.valid) {
        raw.frame_id = raw.aec.frame_id;
        raw.context_id = raw.aec.context_id;
    } else if (raw.awb.valid) {
        raw.frame_id = raw.awb.frame_id;
        raw.context_id = raw.awb.context_id;
    } else {
        raw.frame_id = raw.af.frame_id;
        raw.context_id = raw.af.context_id;
    }
    raw.mean_luma = raw.aec.valid ? isp_mean8(raw.aec.sum, raw.aec.count) : 0U;
    if (raw.awb.valid) {
        raw.mean_red = isp_mean8(raw.awb.sum_r, raw.awb.count);
        raw.mean_green = isp_mean8(raw.awb.sum_g, raw.awb.count);
        raw.mean_blue = isp_mean8(raw.awb.sum_b, raw.awb.count);
    }
    if (raw.af.valid) {
        raw.focus_metric = (raw.af.focus > 0xFFFFU) ? (uint16_t)0xFFFFU : (uint16_t)raw.af.focus;
    }
    *statistics = raw;
    return NEON_OK;
}

neon_status_t neon_isp_set_context_id(neon_isp_handle_t *handle, uint32_t context_id)
{
    if (handle == NULL) {
        return NEON_INVALID_PARAM;
    }
    if (!isp_configured(handle)) {
        return NEON_ERROR;
    }
    return neon_isp_hw_set_context_id(handle, context_id);
}

neon_status_t neon_isp_apply_dynamic_controls(neon_isp_handle_t *handle,
                                              const neon_isp_dynamic_controls_t *controls,
                                              uint32_t *effective_frame_id)
{
    neon_isp_status_t counters;
    neon_status_t status;

    if (handle == NULL || controls == NULL) {
        return NEON_INVALID_PARAM;
    }
    if (controls->wb_red_gain_q8 == 0U || controls->wb_red_gain_q8 > handle->caps.max_wb_gain_q8 ||
        controls->wb_blue_gain_q8 == 0U || controls->wb_blue_gain_q8 > handle->caps.max_wb_gain_q8) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state != NEON_ISP_STATE_RUNNING) {
        return NEON_ERROR;
    }
    status = neon_isp_hw_apply_controls(handle, controls);
    if (status != NEON_OK || effective_frame_id == NULL) {
        return status;
    }
    /*
     * The counter is read after the write, so that a frame that began in between cannot make the answer
     * earlier than the truth: the controls are never later than the frame reported.
     */
    (void)memset(&counters, 0, sizeof(counters));
    status = neon_isp_hw_read_counters(handle, &counters);
    if (status == NEON_OK) {
        *effective_frame_id = counters.frames_in + 1U;
    }
    return status;
}
