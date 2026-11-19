/*
 * Khởi tạo nguội toàn pipeline bằng API từng khối; không cấp quyền DMA hay gửi frame.
 * Thứ tự đọc: cấu hình mặc định -> kiểm tra -> các hàm hỗ trợ -> isp_init().
 */
#include "pipeline/ftel_isp_init.h"
#include "registers/ftel_isp_regs.h"

const char *isp_init_stage_name(isp_init_stage_t stage)
{
    static const char *names[] = {
        [ISP_INIT_VALIDATE] = "VALIDATE",
        [ISP_INIT_ABI] = "ABI",
        [ISP_INIT_IDLE] = "IDLE",
        [ISP_INIT_RESET] = "RESET",
        [ISP_INIT_IFMT] = "IFMT",
        [ISP_INIT_BLC] = "BLC",
        [ISP_INIT_LSC] = "LSC",
        [ISP_INIT_BPC] = "BPC",
        [ISP_INIT_WB] = "WB",
        [ISP_INIT_DG] = "DG",
        [ISP_INIT_DEMOSAIC] = "DEMOSAIC",
        [ISP_INIT_CCM] = "CCM",
        [ISP_INIT_GAMMA] = "GAMMA",
        [ISP_INIT_CSC] = "CSC",
        [ISP_INIT_GTM] = "GTM",
        [ISP_INIT_NR] = "2DNR",
        [ISP_INIT_EE] = "EE",
        [ISP_INIT_CNF] = "CNF",
        [ISP_INIT_AEC] = "AEC",
        [ISP_INIT_AWB] = "AWB",
        [ISP_INIT_AF] = "AF",
        [ISP_INIT_OUTPUT] = "OUTPUT",
        [ISP_INIT_DMA] = "DMA",
        [ISP_INIT_GEOMETRY] = "GEOMETRY",
        [ISP_INIT_READY] = "READY",
    };
    unsigned count = sizeof(names) / sizeof(names[0]);

    if ((unsigned)stage >= count) {
        return "UNKNOWN";
    }
    return names[stage];
}

/* Chỉ điền dữ liệu trong RAM; chưa ghi thanh ghi hay chạy ảnh. */
void isp_get_default_config(isp_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }

    /* 1. Cấu hình xử lý pixel và các bảng mặc định của từng khối. */
    ifmt_get_default_config(&cfg->ifmt);
    blc_get_default_config(&cfg->blc);
    lsc_get_default_init_config(&cfg->lsc);
    bpc_get_default_config(&cfg->bpc);
    wb_get_default_config(&cfg->wb);
    dg_get_default_config(&cfg->dg);
    demosaic_get_default_config(&cfg->demosaic);
    ccm_get_default_config(&cfg->ccm);
    gamma_get_default_config(&cfg->gamma);
    csc_get_default_config(&cfg->csc);
    gtm_get_default_config(&cfg->gtm);
    nr_get_default_config(&cfg->nr);
    ee_get_default_config(&cfg->ee);
    cnf_get_default_config(&cfg->cnf);

    /* 2. Thống kê/đầu ra dùng kích thước hữu dụng sau chuẩn hóa pha Bayer. */
    uint16_t active_width = 0;
    uint16_t active_height = 0;
    (void)ifmt_active_size(&cfg->ifmt, &active_width, &active_height);

    aec_get_default_config(&cfg->aec, active_width, active_height);
    awb_get_default_config(&cfg->awb);
    cfg->awb.num_zone_x = 32;
    cfg->awb.num_zone_y = 24;
    af_get_default_config(&cfg->af);
    output_get_default_config(&cfg->output, active_width, active_height);

    /* 3. IDMA đọc chiều rộng ngoài; ODMA dùng hình học của từng kênh. */
    output_geometry_t geometry[3];
    for (unsigned channel = 0; channel < 3; channel++) {
        (void)output_compute_geometry(&cfg->output.channel[channel], channel,
                                      active_width, active_height, &geometry[channel]);
    }
    dma_get_default_config(&cfg->dma, cfg->ifmt.width, geometry);
}

/* Giữ lại tên khối gây lỗi; toàn bộ hàm này không truy cập MMIO. */
static isp_status_t validate_config(const isp_config_t *cfg, isp_init_result_t *result)
{
    if (cfg == NULL) {
        return ISP_E_PARAM;
    }

    uint16_t active_width;
    uint16_t active_height;
    if (ifmt_active_size(&cfg->ifmt, &active_width, &active_height) != 0) {
        result->stage = ISP_INIT_IFMT;
        return ISP_E_PARAM;
    }
    result->active_width = active_width;
    result->active_height = active_height;

    isp_status_t status;
    result->stage = ISP_INIT_BLC;
    status = blc_validate_config(&cfg->blc);
    if (status != ISP_OK) {
        return status;
    }

    result->stage = ISP_INIT_LSC;
    status = lsc_validate_init_config(&cfg->lsc, active_width, active_height);
    if (status != ISP_OK) {
        return status;
    }

    result->stage = ISP_INIT_BPC;
    status = bpc_validate_config(&cfg->bpc);
    if (status != ISP_OK) {
        return status;
    }

    result->stage = ISP_INIT_WB;
    status = wb_validate_config(&cfg->wb);
    if (status != ISP_OK) {
        return status;
    }

    result->stage = ISP_INIT_DG;
    status = dg_validate_config(&cfg->dg);
    if (status != ISP_OK) {
        return status;
    }

    result->stage = ISP_INIT_DEMOSAIC;
    status = demosaic_validate_config(&cfg->demosaic);
    if (status != ISP_OK) {
        return status;
    }

    result->stage = ISP_INIT_CCM;
    status = ccm_validate_config(&cfg->ccm);
    if (status != ISP_OK) {
        return status;
    }

    result->stage = ISP_INIT_GAMMA;
    status = gamma_validate_config(&cfg->gamma);
    if (status != ISP_OK) {
        return status;
    }

    result->stage = ISP_INIT_CSC;
    status = csc_validate_config(&cfg->csc);
    if (status != ISP_OK) {
        return status;
    }

    result->stage = ISP_INIT_GTM;
    status = gtm_validate_config(&cfg->gtm);
    if (status != ISP_OK) {
        return status;
    }

    result->stage = ISP_INIT_NR;
    status = nr_validate_config(&cfg->nr);
    if (status != ISP_OK) {
        return status;
    }

    result->stage = ISP_INIT_EE;
    status = ee_validate_config(&cfg->ee, active_width, active_height);
    if (status != ISP_OK) {
        return status;
    }

    result->stage = ISP_INIT_CNF;
    status = cnf_validate_config(&cfg->cnf);
    if (status != ISP_OK) {
        return status;
    }

    result->stage = ISP_INIT_AEC;
    status = aec_validate_config(&cfg->aec, active_width, active_height);
    if (status != ISP_OK) {
        return status;
    }

    result->stage = ISP_INIT_AWB;
    status = awb_validate_config(&cfg->awb, active_width, active_height);
    if (status != ISP_OK) {
        return status;
    }

    result->stage = ISP_INIT_AF;
    status = af_validate_config(&cfg->af);
    if (status != ISP_OK) {
        return status;
    }

    result->stage = ISP_INIT_OUTPUT;
    status = output_validate_config(&cfg->output, active_width, active_height);
    if (status != ISP_OK) {
        return status;
    }

    output_geometry_t geometry[3];
    for (unsigned channel = 0; channel < 3; channel++) {
        (void)output_compute_geometry(&cfg->output.channel[channel], channel,
                                      active_width, active_height, &geometry[channel]);
    }

    result->stage = ISP_INIT_DMA;
    status = dma_validate_config(&cfg->dma, cfg->ifmt.width, cfg->ifmt.height, geometry);
    if (status != ISP_OK) {
        return status;
    }

    return ISP_OK;
}

isp_status_t isp_validate_config(const isp_config_t *cfg)
{
    isp_init_result_t result = {0};
    return validate_config(cfg, &result);
}

/* Chỉ đánh dấu tiến độ phần mềm; không điều khiển khối phần cứng. */
static void mark_stage_complete(isp_init_result_t *result)
{
    result->completed |= 1u << result->stage;
}

static isp_status_t check_idle_for_init(void)
{
    /* Giữ nguyên thứ tự đọc: pipeline/DMA, LSC, đầu ra, rồi các lease 3A. */
    if (isp_not_idle() ||
        (REG_RD(R_LSC_LOAD_STATUS) & LSC_LOAD_STATUS_BUSY) ||
        (REG_RD(FX1_ISP_ODMA_BUF_FREE_OFFSET) & 15u) ||
        (REG_RD(FX1_ISP_CH1_ODMA_BUF_FREE_OFFSET) & 15u) ||
        (REG_RD(FX1_ISP_CH2_ODMA_BUF_FREE_OFFSET) & 15u) ||
        (REG_RD(R_TUNE_AEC_STATUS) & 1u) ||
        (REG_RD(FX1_ISP_TUNE_AF_STATUS_OFFSET) & 1u) ||
        (REG_RD(R_AWB_RESULT_STATUS) & AWB_RESULT_STATUS_LEASED)) {
        return ISP_E_NOT_IDLE;
    }
    return ISP_OK;
}

static isp_status_t reset_pipeline(const isp_wait_t *wait)
{
    REG_WR(R_DMA_CTRL, DMA_CTRL_RESET_VALUE);
    REG_WR(R_COMMON_IRQ_EN, 0);
    REG_WR(R_DMA_IRQ_EN, 0);

    /* DONE là W1C: xóa kết quả cũ trước khi gửi yêu cầu reset mới. */
    REG_WR(R_COMMON_RECOVERY_STATUS, COMMON_RECOVERY_STATUS_DONE);
    isp_barrier();
    REG_WR(R_COMMON_CTRL, COMMON_CTRL_SOFT_RST);
    isp_barrier();

    /* Mặt nạ 3 gồm BUSY và DONE; chỉ thành công khi BUSY=0, DONE=1. */
    return isp_wait_register(R_COMMON_RECOVERY_STATUS, 3u,
                             COMMON_RECOVERY_STATUS_DONE, wait);
}

/* Gọi tuần tự từng driver; lỗi ở đâu thì giữ stage đó và dừng tại đó. */
static isp_status_t configure_pipeline_blocks(const isp_config_t *cfg,
                                             const isp_wait_t *wait,
                                             isp_init_result_t *result)
{
    uint16_t active_width = result->active_width;
    uint16_t active_height = result->active_height;
    isp_status_t status;

    result->stage = ISP_INIT_IFMT;
    status = ifmt_configure(&cfg->ifmt);
    if (status != ISP_OK) {
        return status;
    }
    mark_stage_complete(result);

    result->stage = ISP_INIT_BLC;
    status = blc_configure(&cfg->blc);
    if (status != ISP_OK) {
        return status;
    }
    mark_stage_complete(result);

    result->stage = ISP_INIT_LSC;
    status = lsc_initialize_ex(&cfg->lsc, active_width, active_height, wait, &result->lsc);
    if (status != ISP_OK) {
        return status;
    }
    mark_stage_complete(result);

    result->stage = ISP_INIT_BPC;
    status = bpc_configure(&cfg->bpc);
    if (status != ISP_OK) {
        return status;
    }
    mark_stage_complete(result);

    result->stage = ISP_INIT_WB;
    status = wb_configure(&cfg->wb);
    if (status != ISP_OK) {
        return status;
    }
    mark_stage_complete(result);

    result->stage = ISP_INIT_DG;
    status = dg_configure(&cfg->dg);
    if (status != ISP_OK) {
        return status;
    }
    mark_stage_complete(result);

    result->stage = ISP_INIT_DEMOSAIC;
    status = demosaic_configure(&cfg->demosaic);
    if (status != ISP_OK) {
        return status;
    }
    mark_stage_complete(result);

    result->stage = ISP_INIT_CCM;
    status = ccm_configure(&cfg->ccm);
    if (status != ISP_OK) {
        return status;
    }
    mark_stage_complete(result);

    result->stage = ISP_INIT_GAMMA;
    status = gamma_configure(&cfg->gamma);
    if (status != ISP_OK) {
        return status;
    }
    mark_stage_complete(result);

    result->stage = ISP_INIT_CSC;
    status = csc_configure(&cfg->csc);
    if (status != ISP_OK) {
        return status;
    }
    mark_stage_complete(result);

    result->stage = ISP_INIT_GTM;
    status = gtm_configure(&cfg->gtm);
    if (status != ISP_OK) {
        return status;
    }
    mark_stage_complete(result);

    result->stage = ISP_INIT_NR;
    status = nr_configure(&cfg->nr);
    if (status != ISP_OK) {
        return status;
    }
    mark_stage_complete(result);

    result->stage = ISP_INIT_EE;
    status = ee_configure(&cfg->ee, active_width, active_height);
    if (status != ISP_OK) {
        return status;
    }
    mark_stage_complete(result);

    result->stage = ISP_INIT_CNF;
    status = cnf_configure(&cfg->cnf);
    if (status != ISP_OK) {
        return status;
    }
    mark_stage_complete(result);

    result->stage = ISP_INIT_AEC;
    status = aec_configure(&cfg->aec, active_width, active_height);
    if (status != ISP_OK) {
        return status;
    }
    mark_stage_complete(result);

    result->stage = ISP_INIT_AWB;
    status = awb_configure(&cfg->awb, active_width, active_height);
    if (status != ISP_OK) {
        return status;
    }
    mark_stage_complete(result);

    result->stage = ISP_INIT_AF;
    status = af_configure(&cfg->af);
    if (status != ISP_OK) {
        return status;
    }
    mark_stage_complete(result);

    return ISP_OK;
}

static isp_status_t configure_outputs_and_dma(const isp_config_t *cfg,
                                            isp_init_result_t *result)
{
    uint16_t active_width = result->active_width;
    uint16_t active_height = result->active_height;
    isp_status_t status;

    result->stage = ISP_INIT_OUTPUT;
    status = output_configure(&cfg->output, active_width, active_height);
    if (status != ISP_OK) {
        return status;
    }
    mark_stage_complete(result);

    output_geometry_t geometry[3];
    for (unsigned channel = 0; channel < 3; channel++) {
        (void)output_compute_geometry(&cfg->output.channel[channel], channel,
                                      active_width, active_height, &geometry[channel]);
    }

    result->stage = ISP_INIT_DMA;
    status = dma_configure(&cfg->dma, cfg->ifmt.width, cfg->ifmt.height, geometry);
    if (status != ISP_OK) {
        return status;
    }
    mark_stage_complete(result);
    return ISP_OK;
}

isp_status_t isp_init(const isp_config_t *cfg, const isp_wait_t *wait,
                      isp_init_result_t *result)
{
    isp_init_result_t empty_result = {0};
    if (result == NULL) {
        result = &empty_result;
    }
    *result = empty_result;

    /* 1. Kiểm tra cấu hình và cách chờ trước mọi thao tác ghi phần cứng. */
    isp_status_t status = validate_config(cfg, result);
    if (status != ISP_OK) {
        result->status = status;
        return status;
    }
    if (wait != NULL &&
        (wait->poll_limit == 0 || (wait->time_us != NULL && wait->timeout_us == 0))) {
        result->stage = ISP_INIT_VALIDATE;
        result->status = ISP_E_PARAM;
        return ISP_E_PARAM;
    }
    result->completed = 1u << ISP_INIT_VALIDATE;

    /* 2. Xác nhận ABI và quyền cấu hình ở legacy khi thiết bị đã rỗi. */
    result->stage = ISP_INIT_ABI;
    status = isp_check_tune_cap(TUNE_CAP_FULL_V107);
    if (status != ISP_OK) {
        goto failed;
    }
    mark_stage_complete(result);

    result->stage = ISP_INIT_IDLE;
    status = isp_require_legacy();
    if (status != ISP_OK) {
        goto failed;
    }
    mark_stage_complete(result);
    status = check_idle_for_init();
    if (status != ISP_OK) {
        goto failed;
    }

    /* 3. Reset chung rồi chờ yêu cầu reset này hoàn tất. */
    result->stage = ISP_INIT_RESET;
    status = reset_pipeline(wait);
    if (status != ISP_OK) {
        goto failed;
    }
    mark_stage_complete(result);

    /* 4. Cấu hình các khối, nạp bảng, rồi cấu hình đầu ra và DMA. */
    status = configure_pipeline_blocks(cfg, wait, result);
    if (status != ISP_OK) {
        goto failed;
    }
    status = configure_outputs_and_dma(cfg, result);
    if (status != ISP_OK) {
        goto failed;
    }

    /* 5. Bật ISP tạm để kiểm tra hình học; chưa cấp quyền bộ đệm/gửi RAW. */
    result->stage = ISP_INIT_GEOMETRY;
    status = isp_enable_pipeline();
    if (status != ISP_OK) {
        goto failed;
    }
    mark_stage_complete(result);
    status = output_wait_geometry(&cfg->output, result->active_width,
                                  result->active_height, wait);
    if (status != ISP_OK) {
        goto failed;
    }
    mark_stage_complete(result);

    /* 6. Tắt ISP và kiểm tra trạng thái cuối; DMA vẫn chưa chạy frame. */
    result->stage = ISP_INIT_READY;
    status = isp_clear_common_ctrl();
    if (status != ISP_OK) {
        goto failed;
    }
    mark_stage_complete(result);
    if (isp_not_idle() || (REG_RD(R_DMA_CTRL) & 3u) || REG_RD(R_DMA_ERR) ||
        isp_tune_error_set() || (REG_RD(R_COMMON_STATUS) & COMMON_STATUS_ERROR)) {
        status = ISP_E_IO;
        goto failed;
    }

    result->status = ISP_OK;
    return ISP_OK;

failed:
    result->completed &= ~(1u << result->stage);
    /*
     * Giữ nguyên thiết bị nếu lỗi trước reset. Reset hết hạn có thể vẫn BUSY,
     * nên không tự reset lại. Chỉ tắt enable đã bật tạm ở bước hình học;
     * các tham số/bảng đã ghi không được tự hoàn tác.
     */
    if (result->stage == ISP_INIT_GEOMETRY || result->stage == ISP_INIT_READY) {
        isp_clear_common_ctrl();
    }
    result->status = status;
    return status;
}
