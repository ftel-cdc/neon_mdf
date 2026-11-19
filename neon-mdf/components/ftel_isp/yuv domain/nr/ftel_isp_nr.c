/*
 * Cấu hình bật khử nhiễu không gian 2D; bộ ước lượng nhiễu legacy là chỉ đọc.
 * Hiện thực và luồng xử lý.
 */
#include "yuv domain/nr/ftel_isp_nr.h"
#include "registers/ftel_isp_regs.h"

void nr_get_default_config(nr_config_t *cfg)
{
    if (!cfg) return;
    cfg->enable = false;
}

isp_status_t nr_validate_config(const nr_config_t *cfg)
{
    if (!cfg) return ISP_E_PARAM;
    return ISP_OK;
}

isp_status_t nr_configure(const nr_config_t *cfg)
{
    isp_status_t st = nr_validate_config(cfg);
    if (st != ISP_OK) return st;
    const isp_reg_setting_t words[] = {
        {FX1_ISP_NR_2D_CTRL_OFFSET, 1u, cfg->enable ? 1u : 0u}
    };
    return isp_configure_registers(words, sizeof(words) / sizeof(words[0]));
}
