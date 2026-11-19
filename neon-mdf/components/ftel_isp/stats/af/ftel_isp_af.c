/*
 * Cấu hình thống kê lấy nét legacy; chưa triển khai thuật toán AF hay lệnh chạy thấu kính.
 * Hiện thực và luồng xử lý.
 */
#include "stats/af/ftel_isp_af.h"
#include "registers/ftel_isp_regs.h"

void af_get_default_config(af_config_t *cfg)
{
    if (!cfg) return;
    cfg->enable = false;
    cfg->context_id = 0;
}

isp_status_t af_validate_config(const af_config_t *cfg)
{
    if (!cfg) return ISP_E_PARAM;
    return ISP_OK;
}

isp_status_t af_configure(const af_config_t *cfg)
{
    isp_status_t st = af_validate_config(cfg);
    if (st != ISP_OK) return st;
    const isp_reg_setting_t words[] = {
        {FX1_ISP_AF_CONTEXT_ID_OFFSET, 0xffffffffu, cfg->context_id},
        {FX1_ISP_AF_CTRL_OFFSET, 1u, cfg->enable ? 1u : 0u}
    };
    return isp_configure_registers(words, sizeof(words) / sizeof(words[0]));
}
