/*
 * Nạp, đọc lại và xác nhận đủ 256 điểm bảng gamma trước khi bật khối.
 * Hiện thực và luồng xử lý.
 */
#include "rgb domain/gamma/ftel_isp_gamma.h"
#include "registers/ftel_isp_regs.h"

void gamma_get_default_config(gamma_config_t *cfg)
{
    if (!cfg) return;
    cfg->enable = false;
    for (unsigned i = 0; i < GAMMA_LUT_SIZE; i++) cfg->lut[i] = (uint16_t)(i * 4095u / 255u);
}

isp_status_t gamma_validate_config(const gamma_config_t *cfg)
{
    if (!cfg) return ISP_E_PARAM;
    for (unsigned i = 0; i < GAMMA_LUT_SIZE; i++) if (cfg->lut[i] > 4095) return ISP_E_PARAM;
    return ISP_OK;
}

isp_status_t gamma_configure(const gamma_config_t *cfg)
{
    isp_status_t st = gamma_validate_config(cfg);
    if (st != ISP_OK) return st;
    st = isp_require_legacy();
    if (st != ISP_OK) return st;
    if (isp_not_idle() || (REG_RD(FX1_ISP_GAMMA_STATUS_OFFSET) & 1u)) return ISP_E_NOT_IDLE;
    st = isp_prepare_config();
    if (st != ISP_OK) return st;
    REG_WR(FX1_ISP_GAMMA_CTRL_OFFSET, 0); /* Ghi nguyên từ để không phát lại CONFIRM. */
    isp_barrier();
    st = isp_config_status();
    if (st != ISP_OK) return st;
    if (!isp_reg_matches(FX1_ISP_GAMMA_CTRL_OFFSET, 1u, 0u)) return ISP_E_IO;
    REG_WR(FX1_ISP_GAMMA_LUT_ADDR_OFFSET, 0);
    isp_barrier();
    for (unsigned i = 0; i < GAMMA_LUT_SIZE; i++) {
        REG_WR(FX1_ISP_GAMMA_LUT_DATA_OFFSET, cfg->lut[i]);
        isp_barrier();
    }
    for (unsigned i = 0; i < GAMMA_LUT_SIZE; i++) {
        REG_WR(FX1_ISP_GAMMA_LUT_ADDR_OFFSET, i);
        isp_barrier();
        if ((REG_RD(FX1_ISP_GAMMA_LUT_RDATA_OFFSET) & 0xfffu) != cfg->lut[i]) return ISP_E_IO;
    }
    st = isp_config_status();
    if (st != ISP_OK) return st;
    REG_WR(FX1_ISP_GAMMA_CTRL_OFFSET, 1u << 8);
    isp_barrier();
    if (!(REG_RD(FX1_ISP_GAMMA_STATUS_OFFSET) & 2u)) return ISP_E_IO;
    st = isp_config_status();
    if (st != ISP_OK) return st;
    REG_WR(FX1_ISP_GAMMA_CTRL_OFFSET, cfg->enable ? 1u : 0u);
    isp_barrier();
    st = isp_config_status();
    if (st != ISP_OK) return st;
    return isp_reg_matches(FX1_ISP_GAMMA_CTRL_OFFSET, 1u, cfg->enable ? 1u : 0u) ? ISP_OK : ISP_E_IO;
}
