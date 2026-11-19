/*
 * Chọn chuẩn chuyển đổi RGB/YUV dùng chung cho CSC chính và CSC ngược tại CH2.
 * Hiện thực và luồng xử lý.
 */
#include "rgb domain/csc/ftel_isp_csc.h"
#include "registers/ftel_isp_regs.h"

void csc_get_default_config(csc_config_t *cfg)
{
    if (!cfg) return;
    cfg->standard = 0;
}

isp_status_t csc_validate_config(const csc_config_t *cfg)
{
    if (!cfg) return ISP_E_PARAM;
    if (cfg->standard > 1) return ISP_E_PARAM;
    return ISP_OK;
}

isp_status_t csc_configure(const csc_config_t *cfg)
{
    isp_status_t st = csc_validate_config(cfg);
    if (st != ISP_OK) return st;
    const isp_reg_setting_t words[] = {
        {FX1_ISP_CSC_CTRL_OFFSET, 1u, cfg->standard}
    };
    return isp_configure_registers(words, sizeof(words) / sizeof(words[0]));
}
