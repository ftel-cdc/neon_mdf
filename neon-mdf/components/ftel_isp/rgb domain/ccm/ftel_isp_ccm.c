/*
 * Cấu hình ma trận hiệu chỉnh màu có dấu 3x3 và offset RGB.
 * Hiện thực và luồng xử lý.
 */
#include "rgb domain/ccm/ftel_isp_ccm.h"
#include "registers/ftel_isp_regs.h"
void ccm_get_default_config(ccm_config_t *cfg)
{
    if (!cfg) return;
    cfg->enable = false;
    for (unsigned i = 0; i < 9; i++) cfg->matrix[i] = (i % 4 == 0) ? 512 : 0;
    for (unsigned i = 0; i < 3; i++) cfg->offset[i] = 0;
}

isp_status_t ccm_validate_config(const ccm_config_t *cfg)
{
    if (!cfg) return ISP_E_PARAM;
    for (unsigned i = 0; i < 9; i++)
        if (cfg->matrix[i] < -2048 || cfg->matrix[i] > 2047) return ISP_E_PARAM;
    for (unsigned i = 0; i < 3; i++)
        if (cfg->offset[i] < -2048 || cfg->offset[i] > 2047) return ISP_E_PARAM;
    return ISP_OK;
}

isp_status_t ccm_configure(const ccm_config_t *cfg)
{
    isp_status_t st = ccm_validate_config(cfg);
    if (st != ISP_OK) return st;
    isp_reg_setting_t words[13];
    for (unsigned i = 0; i < 9; i++) {
        words[i].address = FX1_ISP_CCM_CRR_OFFSET + 4u * i;
        words[i].mask = 0xfffu;
        words[i].value = (uint32_t)cfg->matrix[i] & 0xfffu;
    }
    for (unsigned i = 0; i < 3; i++) {
        words[9+i].address = FX1_ISP_CCM_OFS_R_OFFSET + 4u * i;
        words[9+i].mask = 0xfffu;
        words[9+i].value = (uint32_t)cfg->offset[i] & 0xfffu;
    }
    words[12] = (isp_reg_setting_t){FX1_ISP_CCM_CTRL_OFFSET, 1u, cfg->enable ? 1u : 0u};
    return isp_configure_registers(words, 13);
}
