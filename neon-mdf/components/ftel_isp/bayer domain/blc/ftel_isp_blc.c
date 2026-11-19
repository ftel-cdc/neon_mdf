/* BLC FX1 ISP-UPD-002: eight profiles, per-channel UQ2.16 scales. */
#include "bayer domain/blc/ftel_isp_blc.h"

/* G4..G7 offsets are not contiguous with G0..G3. */
#define BLC_PROFILE(g) { \
    {FX1_ISP_BLC_OFS_G##g##_DFT_OFFSET, FX1_ISP_BLC_OFS_G##g##_R_OFFSET, \
     FX1_ISP_BLC_OFS_G##g##_GR_OFFSET, FX1_ISP_BLC_OFS_G##g##_GB_OFFSET, \
     FX1_ISP_BLC_OFS_G##g##_B_OFFSET}, \
    {FX1_ISP_BLC_SCALE_G##g##_R_OFFSET, FX1_ISP_BLC_SCALE_G##g##_GR_OFFSET, \
     FX1_ISP_BLC_SCALE_G##g##_GB_OFFSET, FX1_ISP_BLC_SCALE_G##g##_B_OFFSET} }
static const struct { uint16_t ofs[5]; uint16_t scale[4]; } blc_map[BLC_NUM_PROFILES] = {
    BLC_PROFILE(0), BLC_PROFILE(1), BLC_PROFILE(2), BLC_PROFILE(3),
    BLC_PROFILE(4), BLC_PROFILE(5), BLC_PROFILE(6), BLC_PROFILE(7)
};
#undef BLC_PROFILE

uint32_t blc_calc_scale(uint16_t pedestal)
{
    if (pedestal == 0u) return BLC_SCALE_UNITY;
    if (pedestal >= BLC_PEDESTAL_MAX) return BLC_SCALE_MAX;
    uint64_t s = ((uint64_t)BLC_PEDESTAL_MAX << 16) / (BLC_PEDESTAL_MAX - pedestal);
    return s > BLC_SCALE_MAX ? BLC_SCALE_MAX : (uint32_t)s;
}

static uint32_t channel_scale(const blc_profile_config_t *p, unsigned channel, uint16_t trim)
{
    if (p->per_channel_scale) return p->channel_scale[channel];
    if (p->scale) return p->scale;
    /* Shared pedestal + channel trim is a 13-bit sum, without wrap. */
    return blc_calc_scale((uint16_t)(p->dft + trim));
}

void blc_get_default_config(blc_config_t *cfg)
{
    if (!cfg) return;
    *cfg = (blc_config_t){0};
    cfg->range_scale_en = true;
    for (unsigned i = 0; i < BLC_NUM_PROFILES; ++i) cfg->profile[i].dft = 200u;
}

isp_status_t blc_validate_config(const blc_config_t *cfg)
{
    if (!cfg || cfg->gain_level >= BLC_NUM_PROFILES) return ISP_E_PARAM;
    for (unsigned i = 0; i < BLC_NUM_PROFILES; ++i) {
        const blc_profile_config_t *p = &cfg->profile[i];
        if (p->dft > BLC_PEDESTAL_MAX || p->r > BLC_TRIM_MAX || p->gr > BLC_TRIM_MAX ||
            p->gb > BLC_TRIM_MAX || p->b > BLC_TRIM_MAX || p->scale > BLC_SCALE_MAX)
            return ISP_E_PARAM;
        for (unsigned c = 0; c < 4; ++c)
            if (p->channel_scale[c] > BLC_SCALE_MAX) return ISP_E_PARAM;
    }
    return ISP_OK;
}

isp_status_t blc_configure(const blc_config_t *cfg)
{
    if (blc_validate_config(cfg) != ISP_OK) return ISP_E_PARAM;
    isp_status_t st = isp_prepare_config();
    if (st != ISP_OK) return st;
    for (unsigned i = 0; i < BLC_NUM_PROFILES; ++i) {
        const blc_profile_config_t *p = &cfg->profile[i];
        const uint16_t values[5] = {p->dft, p->r, p->gr, p->gb, p->b};
        for (unsigned c = 0; c < 5; ++c) {
            st = isp_write_checked(blc_map[i].ofs[c], BLC_PEDESTAL_MAX, values[c]);
            if (st != ISP_OK) return st;
        }
        for (unsigned c = 0; c < 4; ++c) {
            st = isp_write_checked(blc_map[i].scale[c], BLC_SCALE_MAX, channel_scale(p, c, values[c + 1]));
            if (st != ISP_OK) return st;
        }
    }
    isp_barrier();
    uint32_t sel = BLC_GAIN_SEL_GAIN_LVL(cfg->gain_level);
    if (cfg->range_scale_en) sel |= FX1_ISP_BLC_GAIN_SEL_RANGE_SCALE_EN_MASK;
    st = isp_write_checked(R_BLC_GAIN_SEL,
        FX1_ISP_BLC_GAIN_SEL_GAIN_LEVEL_MASK | FX1_ISP_BLC_GAIN_SEL_RANGE_SCALE_EN_MASK, sel);
    if (st != ISP_OK) return st;
    st = isp_write_checked(R_BLC_CTRL, FX1_ISP_BLC_CTRL_EN_MASK, cfg->enable ? BLC_CTRL_EN : 0u);
    return st == ISP_OK ? isp_config_status() : st;
}
