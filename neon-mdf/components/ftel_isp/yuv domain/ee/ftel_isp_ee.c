/*
 * Cấu hình tăng cường biên và nạp/xác nhận bảng của ba bộ tham số lưu trú.
 * Hiện thực và luồng xử lý.
 */
#include "yuv domain/ee/ftel_isp_ee.h"
#include "registers/ftel_isp_regs.h"
void ee_get_default_config(ee_config_t *cfg)
{
    if (!cfg) return;
    cfg->enable = false;
    cfg->profile = 0;
    for (unsigned p = 0; p < 3; p++) {
        ee_profile_config_t *v = &cfg->resident[p];
        v->alpha = 0x4000;
        v->beta = 0x2000;
        v->clamp_positive = 16;
        v->clamp_negative = 24;
        v->features = 0;
        v->center_x2 = v->center_y2 = 0;
        for (unsigned i = 0; i < 3; i++) v->radius2[i] = 0;
        for (unsigned i = 0; i < 4; i++) v->radial_gain[i] = 0x8000;
        for (unsigned i = 0; i < 64; i++) v->luma[i] = v->activity[i] = 0x8000;
        for (unsigned i = 0; i < 32; i++) v->contrast_positive[i] = v->contrast_negative[i] = 0x8000;
    }
}

isp_status_t ee_validate_config(const ee_config_t *cfg, uint16_t w, uint16_t h)
{
    if (!cfg || cfg->profile > 2 || !w || !h) return ISP_E_PARAM;
    for (unsigned p = 0; p < 3; p++) {
        const ee_profile_config_t *v = &cfg->resident[p];
        if (v->alpha > 0x8000 || v->beta > 0x8000 || v->features > 7 ||
            v->center_x2 > 8191 || v->center_y2 > 8191) return ISP_E_PARAM;
        if ((v->features & 4u) && (v->center_x2 > 2u*(w-1u) || v->center_y2 > 2u*(h-1u) ||
            v->radius2[0] > v->radius2[1] || v->radius2[1] > v->radius2[2])) return ISP_E_PARAM;
        for (unsigned i = 0; i < 3; i++) if (v->radius2[i] > 0x7ffffffu) return ISP_E_PARAM;
        for (unsigned i = 0; i < 4; i++) if (v->radial_gain[i] > 0xc000) return ISP_E_PARAM;
        for (unsigned i = 0; i < 64; i++) if (v->luma[i] > 0x8000 || v->activity[i] > 0x8000) return ISP_E_PARAM;
        for (unsigned i = 0; i < 32; i++)
            if (v->contrast_positive[i] > 0x8000 || v->contrast_negative[i] > 0x8000) return ISP_E_PARAM;
    }
    return ISP_OK;
}

isp_status_t ee_configure(const ee_config_t *cfg, uint16_t w, uint16_t h)
{
    isp_status_t st = ee_validate_config(cfg, w, h);
    if (st != ISP_OK) return st;
    st = isp_require_legacy();
    if (st != ISP_OK) return st;
    if (isp_not_idle() || (REG_RD(FX1_ISP_EE_STATUS_OFFSET) & 1u)) return ISP_E_NOT_IDLE;
    st = isp_prepare_config();
    if (st != ISP_OK) return st;
    REG_WR(FX1_ISP_EE_STATUS_OFFSET, 2u); /* Chỉ xóa ERROR, không ghi vào trạng thái RO. */
    for (unsigned p = 0; p < 3; p++) {
        const ee_profile_config_t *v = &cfg->resident[p];
        st = isp_write_checked(FX1_ISP_EE_CTRL_OFFSET, 0x3fu, p << 4);
        if (st != ISP_OK) return st;
        const isp_reg_setting_t scalars[] = {
            {FX1_ISP_EE_ALPHA_OFFSET, 0xffffu, v->alpha},
            {FX1_ISP_EE_BETA_OFFSET, 0xffffu, v->beta},
            {FX1_ISP_EE_CLAMP_OFFSET, 0xffffu, v->clamp_positive | ((uint32_t)v->clamp_negative << 8)},
            {FX1_ISP_EE_FEATURE_EN_OFFSET, 7u, v->features},
            {FX1_ISP_EE_RADIAL_CENTER_OFFSET, 0x1fff1fffu, v->center_x2 | ((uint32_t)v->center_y2 << 16)},
            {FX1_ISP_EE_RADIAL_R2_TH0_OFFSET, 0x7ffffffu, v->radius2[0]},
            {FX1_ISP_EE_RADIAL_R2_TH1_OFFSET, 0x7ffffffu, v->radius2[1]},
            {FX1_ISP_EE_RADIAL_R2_TH2_OFFSET, 0x7ffffffu, v->radius2[2]},
            {FX1_ISP_EE_RADIAL_GAIN_01_OFFSET, 0xffffffffu, v->radial_gain[0] | ((uint32_t)v->radial_gain[1] << 16)},
            {FX1_ISP_EE_RADIAL_GAIN_23_OFFSET, 0xffffffffu, v->radial_gain[2] | ((uint32_t)v->radial_gain[3] << 16)}
        };
        st = isp_configure_registers(scalars, sizeof(scalars) / sizeof(scalars[0]));
        if (st != ISP_OK) return st;
        const uint16_t *tables[] = {v->luma, v->activity, v->contrast_positive, v->contrast_negative};
        for (unsigned bank = 0; bank < 4; bank++) {
            unsigned count = bank < 2 ? 64 : 32;
            REG_WR(FX1_ISP_EE_LUT_CTRL_OFFSET, bank << 1);
            REG_WR(FX1_ISP_EE_LUT_ADDR_OFFSET, 0);
            isp_barrier();
            for (unsigned i = 0; i < count; i++) { REG_WR(FX1_ISP_EE_LUT_WDATA_OFFSET, tables[bank][i]);
                isp_barrier(); }
            for (unsigned i = 0; i < count; i++) {
                REG_WR(FX1_ISP_EE_LUT_ADDR_OFFSET, i);
                isp_barrier();
                if ((REG_RD(FX1_ISP_EE_LUT_RDATA_OFFSET) & 0xffffu) != tables[bank][i]) return ISP_E_IO;
            }
        }
        st = isp_config_status();
        if (st != ISP_OK) return st;
        REG_WR(FX1_ISP_EE_LUT_CTRL_OFFSET, 1u << 8);
        isp_barrier();
        if (!(REG_RD(FX1_ISP_EE_STATUS_OFFSET) & (1u << (4u+p)))) return ISP_E_IO;
        if (REG_RD(FX1_ISP_EE_STATUS_OFFSET) & 2u) return ISP_E_IO;
        st = isp_config_status();
        if (st != ISP_OK) return st;
    }
    st = isp_config_status();
    if (st != ISP_OK) return st;
    return isp_write_checked(FX1_ISP_EE_CTRL_OFFSET, 0x3fu,
                             ((uint32_t)cfg->profile << 4) | ((uint32_t)cfg->profile << 2) | (cfg->enable ? 1u : 0u));
}
