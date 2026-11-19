/*
 * Điều phối Bayer/AEC: cấu hình mặc định, quyền sở hữu ISP, init giới hạn
 * và đọc thống kê. Init toàn pipeline nằm riêng trong ftel_isp_init.c.
 */
#include "ftel_isp_private.h"

/* Session chỉ do bên sở hữu truy cập; lớp FreeRTOS nhận quyền cho worker. */
ftel_isp_fx1_session_t ftel_isp_fx1_session;
static bool (*owner_allowed)(void *);
static void *owner_user;

bool ftel_isp_fx1_access_allowed(void)
{
    return owner_allowed == NULL || owner_allowed(owner_user);
}

void ftel_isp_fx1_set_owner(bool (*allowed)(void *), void *user)
{
    owner_allowed = allowed;
    owner_user = user;
}

ftel_isp_status_t ftel_isp_get_info(ftel_isp_info_t *info)
{
    if (!ftel_isp_fx1_access_allowed()) return FTEL_ISP_E_BUSY;
    if (info == NULL) return FTEL_ISP_E_CONFIG;
    *info = (ftel_isp_info_t){
        .version = REG_RD(FX1_ISP_COMMON_VER_ID_OFFSET),
        .date = REG_RD(FX1_ISP_COMMON_VER_DATE_OFFSET),
        .tune_revision = REG_RD(FX1_ISP_TUNE_REV_OFFSET),
        .capabilities = REG_RD(FX1_ISP_TUNE_CAPS_OFFSET),
        .input_count = REG_RD(FX1_ISP_IDMA_FRAME_COUNT_OFFSET),
        .output_count = REG_RD(FX1_ISP_ODMA_FRAME_COUNT_OFFSET),
        .recovery_sequence = REG_RD(FX1_ISP_COMMON_RECOVERY_SEQUENCE_OFFSET),
        .busy = isp_not_idle() || (REG_RD(R_COMMON_CTRL) & COMMON_CTRL_ISP_EN) ||
                (REG_RD(R_DMA_CTRL) & DMA_CTRL_ENABLE_MASK),
    };
    return isp_check_abi() == ISP_OK ? FTEL_ISP_OK : FTEL_ISP_E_ABI;
}

void ftel_isp_get_default_config(ftel_isp_config_t *config)
{
    if (config == NULL) return;
    *config = (ftel_isp_config_t){
        .bayer = {
            .input = {64, 48, FTEL_ISP_RAW12, FTEL_ISP_RGGB},
            .blc = {false, 0},
            .bpc = {false},
            .wb = {false, 256, 256, 256},
            .dg = {false, 256},
        },
        .aec = {true, FTEL_ISP_AEC_AFTER_DG, 1},
    };
}

ftel_isp_status_t ftel_isp_fx1_status(isp_status_t status)
{
    switch (status) {
    case ISP_OK: return FTEL_ISP_OK;
    case ISP_E_PARAM: return FTEL_ISP_E_CONFIG;
    case ISP_E_BAD_ABI: return FTEL_ISP_E_ABI;
    case ISP_E_NOT_IDLE: return FTEL_ISP_E_BUSY;
    case ISP_E_STATE: return FTEL_ISP_E_STATE;
    case ISP_E_NO_DATA: return FTEL_ISP_E_NO_DATA;
    case ISP_E_STALE: return FTEL_ISP_E_STALE;
    case ISP_E_TIMEOUT: return FTEL_ISP_E_TIMEOUT;
    case ISP_E_UNSUPPORTED: return FTEL_ISP_E_UNSUPPORTED;
    default: return FTEL_ISP_E_IO;
    }
}

/* Chuyển cấu hình HAL và cấu hình các khối Bayer/AEC. */
isp_status_t ftel_isp_fx1_prepare_config(const ftel_isp_config_t *config,
                                        ftel_isp_fx1_config_t *backend,
                                        ftel_isp_init_result_t *result)
{
    if (config == NULL || (config->bayer.input.raw_depth != FTEL_ISP_RAW10 &&
                          config->bayer.input.raw_depth != FTEL_ISP_RAW12) ||
        (unsigned)config->bayer.input.pattern > FTEL_ISP_BGGR ||
        (config->aec.source != FTEL_ISP_AEC_AFTER_BPC &&
         config->aec.source != FTEL_ISP_AEC_AFTER_DG)) return ISP_E_PARAM;

    ifmt_get_default_config(&backend->ifmt);
    backend->ifmt.width = config->bayer.input.width;
    backend->ifmt.height = config->bayer.input.height;
    backend->ifmt.depth = config->bayer.input.raw_depth == FTEL_ISP_RAW10 ?
                          INPUT_DEPTH_RAW10 : INPUT_DEPTH_RAW12;
    /* Enum HAL tuân theo thứ tự pha RGGB/GRBG/GBRG/BGGR của IFMT. */
    backend->ifmt.bayer = (bayer_pattern_t)config->bayer.input.pattern;
    if (ifmt_active_size(&backend->ifmt, &result->active_width,
                         &result->active_height) != 0) return ISP_E_PARAM;

    blc_get_default_config(&backend->blc);
    backend->blc.enable = config->bayer.blc.enable;
    backend->blc.range_scale_en = false;
    for (unsigned i = 0; i < BLC_NUM_PROFILES; ++i) backend->blc.profile[i].dft = config->bayer.blc.black_level;
    bpc_get_default_config(&backend->bpc);
    backend->bpc.enable = config->bayer.bpc.enable;
    wb_get_default_config(&backend->wb);
    backend->wb.enable = config->bayer.wb.enable;
    backend->wb.gain_r = config->bayer.wb.r_q8;
    backend->wb.gain_g = config->bayer.wb.g_q8;
    backend->wb.gain_b = config->bayer.wb.b_q8;
    dg_get_default_config(&backend->dg);
    backend->dg.enable = config->bayer.dg.enable;
    backend->dg.gain = config->bayer.dg.gain_q8;
    aec_get_default_config(&backend->aec, result->active_width, result->active_height);
    backend->aec.enable = config->aec.enable;
    backend->aec.source = config->aec.source == FTEL_ISP_AEC_AFTER_DG ? AEC_SOURCE_DG : AEC_SOURCE_BPC;
    backend->aec.context_id = config->aec.context_id;

    if (blc_validate_config(&backend->blc) != ISP_OK ||
        bpc_validate_config(&backend->bpc) != ISP_OK ||
        wb_validate_config(&backend->wb) != ISP_OK ||
        dg_validate_config(&backend->dg) != ISP_OK) return ISP_E_PARAM;
    return aec_validate_config(&backend->aec, result->active_width, result->active_height);
}

isp_status_t ftel_isp_fx1_bayer_init(const ftel_isp_fx1_config_t *config,
                                    ftel_isp_init_result_t *result)
{
    isp_status_t status;
    /* Mỗi dòng lưu tên bước, gọi driver và dừng ngay nếu driver báo lỗi. */
#define INIT_BLOCK(stage_name, call) do { \
    result->stage = (stage_name); \
    status = (call); \
    if (status != ISP_OK) return status; \
} while (0)
    INIT_BLOCK(FTEL_ISP_INIT_IFMT, ifmt_configure(&config->ifmt));
    INIT_BLOCK(FTEL_ISP_INIT_BLC, blc_configure(&config->blc));
    /* Chỉ tắt LSC, không chọn profile chưa valid hoặc nạp bảng test. */
    INIT_BLOCK(FTEL_ISP_INIT_LSC_BYPASS, isp_write_checked(R_LSC_CTRL, LSC_CTRL_EN, 0));
    INIT_BLOCK(FTEL_ISP_INIT_BPC, bpc_configure(&config->bpc));
    INIT_BLOCK(FTEL_ISP_INIT_WB, wb_configure(&config->wb));
    INIT_BLOCK(FTEL_ISP_INIT_DG, dg_configure(&config->dg));
#undef INIT_BLOCK
    return ISP_OK;
}

isp_status_t ftel_isp_fx1_aec_init(const ftel_isp_fx1_config_t *config,
                                  ftel_isp_init_result_t *result)
{
    result->stage = FTEL_ISP_INIT_AEC;
    return aec_configure(&config->aec, result->active_width, result->active_height);
}

/* Reset và init Bayer/AEC; chưa cấp DMA hoặc chạy frame. */
static isp_status_t reset_isp(void)
{
    REG_WR(R_DMA_CTRL, DMA_CTRL_RESET_VALUE);
    REG_WR(R_COMMON_IRQ_EN, 0);
    REG_WR(R_DMA_IRQ_EN, 0);
    /* Xóa DONE cũ rồi mới yêu cầu soft reset; chờ BUSY=0, DONE=1. */
    REG_WR(R_COMMON_RECOVERY_STATUS, COMMON_RECOVERY_STATUS_DONE);
    isp_barrier();
    REG_WR(R_COMMON_CTRL, COMMON_CTRL_SOFT_RST);
    isp_barrier();
    return isp_wait_register(R_COMMON_RECOVERY_STATUS, 3u,
                             COMMON_RECOVERY_STATUS_DONE, NULL);
}

ftel_isp_status_t ftel_isp_init_bayer_aec(const ftel_isp_config_t *config,
                                        ftel_isp_init_result_t *result)
{
    if (!ftel_isp_fx1_access_allowed()) return FTEL_ISP_E_BUSY;
    ftel_isp_init_result_t local;
    if (result == NULL) result = &local;
    *result = (ftel_isp_init_result_t){.stage = FTEL_ISP_INIT_VALIDATE};
    ftel_isp_fx1_config_t backend;

    /* Mọi cấu hình được kiểm tra trong RAM trước lần ghi MMIO đầu tiên. */
    isp_status_t status = ftel_isp_fx1_prepare_config(config, &backend, result);
    if (status != ISP_OK) goto done;
    result->stage = FTEL_ISP_INIT_ABI;
    status = isp_require_legacy();
    if (status != ISP_OK) goto done;
    result->stage = FTEL_ISP_INIT_IDLE;
    if (isp_not_idle() || (REG_RD(R_COMMON_CTRL) & COMMON_CTRL_ISP_EN) ||
        (REG_RD(R_DMA_CTRL) & DMA_CTRL_ENABLE_MASK) ||
        (REG_RD(R_TUNE_AEC_STATUS) & TUNE_AEC_STATUS_LEASE_VALID)) {
        status = ISP_E_NOT_IDLE;
        goto done;
    }
    result->stage = FTEL_ISP_INIT_RESET;
    /* Clock/DDR/reset nền tảng đã do BSP VP chuẩn bị; chưa có driver CMU silicon.
     * Chỉ tới đây mới thay trạng thái phần mềm vì chuẩn bị ghi soft reset IP. */
    ftel_isp_fx1_session = (ftel_isp_fx1_session_t){0};
    status = reset_isp();
    if (status != ISP_OK) goto done;
    status = ftel_isp_fx1_bayer_init(&backend, result);
    if (status != ISP_OK) goto done;
    status = ftel_isp_fx1_aec_init(&backend, result);
    if (status != ISP_OK) goto done;
    result->stage = FTEL_ISP_INIT_CHECK;
    status = isp_config_status();
    if (status != ISP_OK) goto done;
    if ((REG_RD(R_COMMON_CTRL) & COMMON_CTRL_ISP_EN) != 0 ||
        (REG_RD(R_DMA_CTRL) & DMA_CTRL_ENABLE_MASK) != 0) {
        status = ISP_E_IO;
        goto done;
    }
    result->stage = FTEL_ISP_INIT_READY;
    ftel_isp_fx1_session = (ftel_isp_fx1_session_t){
        .ready = true, .config = *config,
        .active_width = result->active_width, .active_height = result->active_height,
        .recovery_sequence = REG_RD(R_COMMON_RECOVERY_SEQUENCE),
    };
done:
    result->driver_status = status;
    return ftel_isp_fx1_status(status);
}

/* Đọc thống kê của frame đã hoàn thành và kiểm tra nhãn/nguồn đo. */
ftel_isp_status_t ftel_isp_read_aec(ftel_isp_aec_stats_t *stats)
{
    if (!ftel_isp_fx1_access_allowed()) return FTEL_ISP_E_BUSY;
    if (stats == NULL) return FTEL_ISP_E_CONFIG;
    const ftel_isp_fx1_session_t *session = &ftel_isp_fx1_session;
    if (!session->ready) return FTEL_ISP_E_STATE;
    if (!session->complete) return FTEL_ISP_E_NO_DATA;
    if (REG_RD(R_COMMON_RECOVERY_SEQUENCE) != session->recovery_sequence)
        return FTEL_ISP_E_STALE;
    aec_frame_stats_t raw;
    isp_status_t status = aec_get_stats(&raw);
    if (status != ISP_OK) return ftel_isp_fx1_status(status);
    if (raw.global.frame_id != session->frame_id ||
        raw.global.context_id != session->config.aec.context_id ||
        (unsigned)raw.source != (unsigned)session->config.aec.source) return FTEL_ISP_E_STALE;
    *stats = (ftel_isp_aec_stats_t){
        .frame_id = raw.global.frame_id, .context_id = raw.global.context_id,
        .source = (ftel_isp_aec_source_t)raw.source,
        .sum = raw.global.global_sum, .count = raw.global.global_count,
        .ue_count = raw.global.ue_count, .oe_count = raw.global.oe_count,
        .overflow = raw.global.overflow,
    };
    for (unsigned i = 0; i < FTEL_ISP_AEC_HIST_BINS; ++i) stats->histogram[i] = raw.histogram[i];
    return FTEL_ISP_OK;
}
