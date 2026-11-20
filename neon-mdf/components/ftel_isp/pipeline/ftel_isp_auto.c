/** @file ftel_isp_auto.c @brief Binary 3A lifetime and idle WB integration. */
#include "ftel_isp_auto.h"
#include "ftel_isp_auto_private.h"
#include "ftel_isp_private.h"
#include <string.h>

static size_t auto_offset(void) {
    size_t alignment = fpt_3a_context_alignment();
    return (sizeof(ftel_auto_workspace_t) + alignment - 1u) / alignment * alignment;
}
size_t ftel_isp_auto_memory_size(void) {
    return auto_offset() + fpt_3a_context_size();
}
size_t ftel_isp_auto_memory_alignment(void) {
    size_t vendor = fpt_3a_context_alignment();
    return vendor > _Alignof(ftel_auto_workspace_t) ? vendor : _Alignof(ftel_auto_workspace_t);
}
static void *auto_algorithm(ftel_isp_auto_t *handle) {
    return (uint8_t *)handle->memory + auto_offset();
}
static ftel_auto_workspace_t *auto_workspace(ftel_isp_auto_t *handle) {
    if (!handle || !handle->memory || handle->memory_bytes < ftel_isp_auto_memory_size() ||
        (uintptr_t)handle->memory % ftel_isp_auto_memory_alignment()) {
        return NULL;
    }
    ftel_auto_workspace_t *w = handle->memory;
    return w->magic == FTEL_AUTO_MAGIC ? w : NULL;
}
static ftel_isp_status_t auto_status(fpt_3a_status_t status) {
    switch (status) {
    case FPT_3A_OK: return FTEL_ISP_OK;
    case FPT_3A_E_MODE: return FTEL_ISP_E_UNSUPPORTED;
    case FPT_3A_E_STATS: return FTEL_ISP_E_IO;
    case FPT_3A_E_STATE: return FTEL_ISP_E_STATE;
    default: return FTEL_ISP_E_CONFIG;
    }
}

ftel_isp_status_t ftel_isp_auto_init(ftel_isp_auto_t *handle, void *memory, size_t bytes,
    const void *sensor_bin, size_t sensor_bytes, const void *tuning_bin, size_t tuning_bytes,
    uint32_t flags, isp_config_t *config, ftel_isp_auto_controls_t *initial) {
    if (!handle || !memory || !config || !initial || bytes < ftel_isp_auto_memory_size() ||
        (uintptr_t)memory % ftel_isp_auto_memory_alignment()) {
        return FTEL_ISP_E_CONFIG;
    }
    uint16_t width, height;
    if (ifmt_active_size(&config->ifmt, &width, &height) != ISP_OK) {
        return FTEL_ISP_E_CONFIG;
    }
    ftel_auto_workspace_t *w = memory;
    memset(w, 0, sizeof(*w));
    *handle = (ftel_isp_auto_t){memory, bytes};
    fpt_3a_config_t c = {.abi_version = FPT_3A_ABI_VERSION,
        .width = config->ifmt.width, .height = config->ifmt.height,
        .raw_bits = config->ifmt.depth == INPUT_DEPTH_RAW12 ? 12u : 10u,
        .bayer_pattern = config->ifmt.bayer, .flags = flags};
    ftel_isp_status_t status = auto_status(fpt_3a_init(auto_algorithm(handle),
        bytes - auto_offset(), &c, sensor_bin, sensor_bytes, tuning_bin, tuning_bytes, &w->info));
    if (status != FTEL_ISP_OK) {
        return status;
    }
    isp_config_t proposed = *config;
    aec_get_default_config(&proposed.aec, width, height);
    proposed.aec.enable = true;
    proposed.aec.source = AEC_SOURCE_BPC;
    proposed.aec.context_id = 1u;
    proposed.aec.zones = (aec_zone_config_t){w->info.aec_columns, w->info.aec_rows,
        (width + w->info.aec_columns - 1u) / w->info.aec_columns,
        (height + w->info.aec_rows - 1u) / w->info.aec_rows};
    proposed.aec.sample_min_clip = 0u;
    proposed.aec.sample_max_clip = 4095u;
    proposed.awb = (awb_stat_config_t){.enable = true,
        .num_zone_x = w->info.awb_columns, .num_zone_y = w->info.awb_rows,
        .underexposed_limit = w->info.awb_ue_raw12, .saturation_limit = w->info.awb_oe_raw12,
        .context_id = 1u};
    proposed.wb = (wb_config_t){.enable = true,
        .gain_r = w->info.initial.wb_red_q8, .gain_g = 256u,
        .gain_b = w->info.initial.wb_blue_q8};
    if (aec_validate_config(&proposed.aec, width, height) != ISP_OK ||
        awb_validate_config(&proposed.awb, width, height) != ISP_OK ||
        wb_validate_config(&proposed.wb) != ISP_OK) {
        fpt_3a_reset(auto_algorithm(handle));
        return FTEL_ISP_E_CONFIG;
    }
    w->width = width; w->height = height; w->token = 1u;
    w->stats.aec_zones = w->aec; w->stats.awb_zones = w->awb;
    w->stats.histogram = w->histogram;
    *initial = (ftel_isp_auto_controls_t){w->info.initial.exposure_lines,
        w->info.initial.analog_gain_q8, w->info.initial.wb_red_q8, w->info.initial.wb_blue_q8};
    *config = proposed;
    w->magic = FTEL_AUTO_MAGIC;
    return FTEL_ISP_OK;
}

ftel_isp_status_t ftel_isp_auto_process(ftel_isp_auto_t *handle,
    const ftel_isp_auto_controls_t *effective, ftel_isp_auto_result_t *result) {
    ftel_auto_workspace_t *w = auto_workspace(handle);
    if (!w || !effective || !result) {
        return FTEL_ISP_E_CONFIG;
    }
    if (!ftel_isp_fx1_access_allowed()) {
        return FTEL_ISP_E_BUSY;
    }
    uint32_t recovery = REG_RD(R_COMMON_RECOVERY_SEQUENCE);
    if (w->recovery_bound && recovery != w->recovery_sequence) {
        return FTEL_ISP_E_STALE;
    }
    w->recovery_sequence = recovery;
    w->recovery_bound = true;
    ftel_isp_status_t status = ftel_isp_auto_collect(w);
    if (status != FTEL_ISP_OK) {
        return status;
    }
    fpt_3a_controls_t current = {effective->exposure_lines, effective->analog_gain_q8,
        effective->wb_red_q8, effective->wb_blue_q8};
    fpt_3a_result_t next;
    status = auto_status(fpt_3a_process(auto_algorithm(handle), &w->stats, &current, &next));
    if (status != FTEL_ISP_OK) {
        return status;
    }
    *result = (ftel_isp_auto_result_t){.frame_id = next.frame_id, .context_id = next.context_id,
        .flags = next.flags, .current_luma = next.current_luma,
        .lux_index_q16_16 = next.lux_index_q16_16, .cct_kelvin = next.cct_kelvin,
        .awb_valid_rois = next.awb_valid_rois,
        .proposed = {next.proposed.exposure_lines, next.proposed.analog_gain_q8,
                     next.proposed.wb_red_q8, next.proposed.wb_blue_q8}};
    w->have_frame = true; w->last_frame = next.frame_id;
    w->recovery_sequence = recovery;
    w->aec_ready = w->awb_ready = false;
    return FTEL_ISP_OK;
}

ftel_isp_status_t ftel_isp_auto_apply_wb(ftel_isp_auto_t *handle,
    const ftel_isp_auto_result_t *result, ftel_isp_auto_controls_t *effective) {
    ftel_auto_workspace_t *w = auto_workspace(handle);
    if (!w || !result || !effective || !w->have_frame || result->frame_id != w->last_frame ||
        result->context_id != w->token) {
        return FTEL_ISP_E_STATE;
    }
    if (!ftel_isp_fx1_access_allowed()) {
        return FTEL_ISP_E_BUSY;
    }
    if (REG_RD(R_COMMON_RECOVERY_SEQUENCE) != w->recovery_sequence) {
        return FTEL_ISP_E_STALE;
    }
    if (isp_not_idle() || aec_lease_valid() ||
        (REG_RD(R_AWB_RESULT_STATUS) & AWB_RESULT_STATUS_LEASED)) {
        return FTEL_ISP_E_BUSY;
    }
    wb_config_t wb = {.enable = true, .gain_r = result->proposed.wb_red_q8,
        .gain_g = 256u, .gain_b = result->proposed.wb_blue_q8};
    isp_status_t status = wb_validate_config(&wb);
    if (status != ISP_OK) {
        return ftel_isp_fx1_status(status);
    }
    status = wb_configure(&wb);
    if (status == ISP_OK) {
        uint32_t token = w->token + 1u;
        REG_WR(R_AEC_CONTEXT_ID, token);
        REG_SET(R_AEC_CTRL, AEC_CTRL_COMMIT);
        REG_WR(R_AWB_CONTEXT_ID, token);
        isp_barrier();
        status = isp_config_status();
        if (status == ISP_OK) {
            w->token = token;
            effective->wb_red_q8 = wb.gain_r;
            effective->wb_blue_q8 = wb.gain_b;
        }
    }
    if (status != ISP_OK) {
        w->magic = 0u;
    }
    return ftel_isp_fx1_status(status);
}

void ftel_isp_auto_reset(ftel_isp_auto_t *handle) {
    if (auto_workspace(handle)) {
        fpt_3a_reset(auto_algorithm(handle));
        ((ftel_auto_workspace_t *)handle->memory)->magic = 0u;
    }
    if (handle) {
        *handle = (ftel_isp_auto_t){0};
    }
}
