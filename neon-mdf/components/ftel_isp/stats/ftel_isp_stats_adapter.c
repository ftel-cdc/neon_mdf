/** @file ftel_isp_stats_adapter.c @brief Copy leased FX1 statistics for binary 3A. */
#include "ftel_isp_auto_private.h"
#include "ftel_isp_private.h"

static ftel_isp_status_t auto_collect_aec(ftel_auto_workspace_t *w) {
    if (aec_lease_valid()) {
        return FTEL_ISP_E_BUSY;
    }
    isp_status_t status = aec_acquire();
    if (status != ISP_OK) {
        /* Some reader failures happen after acquire; release only our lease. */
        if (aec_lease_valid()) {
            (void)aec_release();
        }
        return ftel_isp_fx1_status(status);
    }
    aec_global_stats_t global;
    aec_source_t source;
    status = aec_read_global(&global);
    if (status == ISP_OK) {
        status = aec_get_result_source(&source);
    }
    if (status == ISP_OK && (global.overflow || source != AEC_SOURCE_BPC ||
        global.global_count > UINT32_MAX || global.ue_count > UINT32_MAX ||
        global.oe_count > UINT32_MAX)) {
        status = ISP_E_IO;
    }
    if (status == ISP_OK && (global.context_id != w->token ||
        (w->have_frame && (int32_t)(global.frame_id - w->last_frame) <= 0))) {
        status = ISP_E_STALE;
    }
    uint32_t grid = REG_RD(R_TUNE_AE_GRID), size = REG_RD(R_TUNE_AE_ZONE_SIZE);
    uint32_t frame_size = REG_RD(R_TUNE_AE_FRAME_SIZE);
    uint32_t nx = grid & 0x3fu, ny = (grid >> 16) & 0x1fu;
    uint32_t zw = size & 0xffffu, zh = size >> 16;
    if (status == ISP_OK && (nx != w->info.aec_columns || ny != w->info.aec_rows ||
        (frame_size & 0x1fffu) != w->width || ((frame_size >> 16) & 0x1fffu) != w->height ||
        !zw || !zh || (nx - 1u) * zw >= w->width || nx * zw < w->width ||
        (ny - 1u) * zh >= w->height || ny * zh < w->height)) {
        status = ISP_E_IO;
    }
    for (uint32_t i = 0; status == ISP_OK && i < nx * ny; ++i) {
        aec_zone_stats_t raw;
        status = aec_read_zone((uint16_t)i, &raw);
        if (status != ISP_OK) {
            break;
        }
        if (raw.overflow || raw.count > UINT32_MAX || raw.ue_count > UINT32_MAX || raw.oe_count > UINT32_MAX) {
            status = ISP_E_OVERFLOW;
            break;
        }
        uint32_t x = (i % nx) * zw, y = (i / nx) * zh;
        uint32_t width = w->width - x < zw ? w->width - x : zw;
        uint32_t height = w->height - y < zh ? w->height - y : zh;
        w->aec[i] = (fpt_3a_aec_zone_t){raw.sum, (uint32_t)raw.count,
            width * height, (uint32_t)raw.ue_count, (uint32_t)raw.oe_count};
    }
    for (uint32_t i = 0; status == ISP_OK && i < FPT_3A_HIST_BINS; ++i) {
        uint64_t count;
        status = aec_read_hist64((uint8_t)i, &count);
        if (status == ISP_OK && count > UINT32_MAX) {
            status = ISP_E_OVERFLOW;
        }
        if (status == ISP_OK) {
            w->histogram[i] = (uint32_t)count;
        }
    }
    isp_status_t released = aec_release();
    if (released != ISP_OK) {
        return ftel_isp_fx1_status(released);
    }
    if (status != ISP_OK) {
        return ftel_isp_fx1_status(status);
    }
    w->stats.frame_id = global.frame_id;
    w->stats.context_id = global.context_id;
    w->stats.aec_sum = global.global_sum;
    w->stats.aec_count = (uint32_t)global.global_count;
    w->stats.sample_count = (uint32_t)w->width * w->height;
    w->stats.ue_count = (uint32_t)global.ue_count;
    w->stats.oe_count = (uint32_t)global.oe_count;
    w->stats.aec_zone_count = nx * ny;
    w->aec_ready = true;
    return FTEL_ISP_OK;
}

static ftel_isp_status_t auto_collect_awb(ftel_auto_workspace_t *w) {
    awb_lease_status_t lease;
    awb_get_lease_status(&lease);
    if (lease.leased) {
        return FTEL_ISP_E_BUSY;
    }
    if (lease.overrun) {
        return FTEL_ISP_E_IO;
    }
    isp_status_t status = awb_acquire();
    if (status != ISP_OK) {
        awb_get_lease_status(&lease);
        if (lease.leased) {
            (void)awb_release();
        }
        return ftel_isp_fx1_status(status);
    }
    awb_global_stats_t global;
    status = awb_read_global(&global);
    awb_get_lease_status(&lease);
    if (status == ISP_OK && (global.frame_id != w->stats.frame_id ||
        global.context_id != w->stats.context_id)) {
        status = ISP_E_STALE;
    }
    uint32_t nx = lease.num_zone_x, ny = lease.num_zone_y;
    if (status == ISP_OK && (nx != w->info.awb_columns || ny != w->info.awb_rows)) {
        status = ISP_E_IO;
    }
    for (uint32_t i = 0; status == ISP_OK && i < nx * ny; ++i) {
        awb_zone_stats_t raw;
        status = awb_read_zone((uint16_t)i, &raw);
        if (status != ISP_OK) {
            break;
        }
        uint32_t x = i % nx, y = i / nx;
        uint32_t cells_x = w->width / 2u, cells_y = w->height / 2u;
        uint32_t x0 = (x * cells_x + nx - 1u) / nx;
        uint32_t x1 = ((x + 1u) * cells_x + nx - 1u) / nx;
        uint32_t y0 = (y * cells_y + ny - 1u) / ny;
        uint32_t y1 = ((y + 1u) * cells_y + ny - 1u) / ny;
        w->awb[i] = (fpt_3a_awb_zone_t){.red_sum = raw.sum_r,
            .green_sum = raw.sum_g, .blue_sum = raw.sum_b, .count = raw.count,
            .sample_count = 4u * (x1 - x0) * (y1 - y0)};
    }
    isp_status_t released = awb_release();
    if (released != ISP_OK) {
        return ftel_isp_fx1_status(released);
    }
    if (status != ISP_OK) {
        return ftel_isp_fx1_status(status);
    }
    w->stats.awb_zone_count = nx * ny;
    /* CSR v1.07 does not expose scalar UE/OE counts per AWB ROI. Never
     * substitute qualified RGB counts or AEC ROI counters for those values. */
    w->stats.awb_populations_valid = 0u;
    w->awb_ready = true;
    return FTEL_ISP_OK;
}

ftel_isp_status_t ftel_isp_auto_collect(ftel_auto_workspace_t *w) {
    ftel_isp_status_t status;
    if (!w->aec_ready) {
        status = auto_collect_aec(w);
        if (status != FTEL_ISP_OK) {
            return status;
        }
    }
    if (!w->awb_ready) {
        status = auto_collect_awb(w);
        if (status != FTEL_ISP_OK) {
            return status;
        }
    }
    return FTEL_ISP_OK;
}
