/*
 * Cấu hình ánh xạ tông toàn cục và nạp/đọc lại bảng 65 tỷ số.
 * Hiện thực và luồng xử lý.
 */
#include "yuv domain/gtm/ftel_isp_gtm.h"
#include "registers/ftel_isp_regs.h"
void gtm_get_default_config(gtm_config_t *cfg)
{
    if (!cfg) return;
    cfg->enable = false;
    cfg->manual = false;
    cfg->key = 0x2e;
    cfg->white = 0x400;
    cfg->roi_log2 = 11;
    for (unsigned i = 0; i < GTM_LUT_SIZE; i++) cfg->lut[i] = 256;
}

isp_status_t gtm_validate_config(const gtm_config_t *cfg)
{
    return cfg && cfg->key <= 255 && cfg->white && cfg->roi_log2 <= 31 ? ISP_OK : ISP_E_PARAM;
}

isp_status_t gtm_configure(const gtm_config_t *cfg)
{
    isp_status_t st = gtm_validate_config(cfg);
    if (st != ISP_OK) return st;
    st = isp_require_legacy();
    if (st != ISP_OK) return st;
    if (isp_not_idle() || (REG_RD(FX1_ISP_GTM_STATUS_OFFSET) & 1u)) return ISP_E_NOT_IDLE;
    st = isp_prepare_config();
    if (st != ISP_OK) return st;
    REG_WR(FX1_ISP_GTM_CTRL_OFFSET, 2u); /* Tắt GTM, đặt MANUAL trước khi ghi RAM. */
    isp_barrier();
    st = isp_config_status();
    if (st != ISP_OK) return st;
    if (!isp_reg_matches(FX1_ISP_GTM_CTRL_OFFSET, 3u, 2u)) return ISP_E_IO;
    REG_WR(FX1_ISP_GTM_LUT_ADDR_OFFSET, 0);
    isp_barrier();
    for (unsigned i = 0; i < GTM_LUT_SIZE; i++) {
        REG_WR(FX1_ISP_GTM_LUT_DATA_OFFSET, cfg->lut[i]);
        isp_barrier();
    }
    for (unsigned i = 0; i < GTM_LUT_SIZE; i++) {
        REG_WR(FX1_ISP_GTM_LUT_ADDR_OFFSET, i);
        isp_barrier();
        if ((REG_RD(FX1_ISP_GTM_LUT_RDATA_OFFSET) & 0xffffu) != cfg->lut[i]) return ISP_E_IO;
    }
    /* Kiểm tra lỗi LUT trước khi hàm ghi hệ số xóa TUNE_ERROR cũ. */
    st = isp_config_status();
    if (st != ISP_OK) return st;
    const isp_reg_setting_t words[] = {
        {FX1_ISP_GTM_KEY_OFFSET, 0xffu, cfg->key},
        {FX1_ISP_GTM_LWHITE_OFFSET, 0xffffu, cfg->white},
        {FX1_ISP_GTM_ROI_LOG2_OFFSET, 0x1fu, cfg->roi_log2},
        {FX1_ISP_GTM_CTRL_OFFSET, 3u, (cfg->enable ? 1u : 0u) | (cfg->manual ? 2u : 0u)}
    };
    return isp_configure_registers(words, sizeof(words) / sizeof(words[0]));
}
