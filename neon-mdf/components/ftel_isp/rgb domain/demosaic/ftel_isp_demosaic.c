/*
 * Kiểm tra đường nội suy Bayer sang RGB cố định ở legacy; không thêm bit bật giả.
 * Hiện thực và luồng xử lý.
 */
#include "rgb domain/demosaic/ftel_isp_demosaic.h"
#include "registers/ftel_isp_regs.h"

void demosaic_get_default_config(demosaic_config_t *cfg)
{
    if (!cfg) return;
    cfg->green_margin = 0;
    cfg->rb_margin = 0;
}

isp_status_t demosaic_validate_config(const demosaic_config_t *cfg)
{
    if (!cfg) return ISP_E_PARAM;
    if (cfg->green_margin || cfg->rb_margin) return ISP_E_UNSUPPORTED;
    return ISP_OK;
}

isp_status_t demosaic_configure(const demosaic_config_t *cfg)
{
    isp_status_t st = demosaic_validate_config(cfg);
    if (st != ISP_OK) return st;
    const isp_reg_setting_t words[] = {
        {FX1_ISP_TUNE_DEMOSAIC_MARGIN_OFFSET, 0xffffffffu, 0}
    };
    return isp_configure_registers(words, sizeof(words) / sizeof(words[0]));
}
