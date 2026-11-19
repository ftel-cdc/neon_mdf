/*
 * Cấu hình crop/thu nhỏ/định dạng ba kênh và kiểm tra hình học đầu ra đã hợp lệ.
 * Hiện thực và luồng xử lý.
 */
#include "io/output/ftel_isp_output.h"
#include "registers/ftel_isp_regs.h"
static const uint16_t targets[22][2] = {
    {0,0}, {3840,2160}, {2560,1440}, {2880,1620}, {2304,1296}, {1920,1080},
    {1280,720}, {960,540}, {640,360}, {2592,1944}, {2048,1536}, {1600,1200},
    {1280,960}, {800,600}, {640,480}, {0,0}, {640,640}, {640,384}, {640,352},
    {512,512}, {416,416}, {320,320}
};
static const uint32_t resizer_base[3] = {0x2000u,0x3200u,0x3400u};
static const uint32_t crop_base[3] = {0x8b40u,0x8b80u,0x8bc0u};

void output_get_default_config(output_config_t *cfg, uint16_t w, uint16_t h)
{
    if (!cfg) return;
    cfg->done_irq = cfg->error_irq = 0;
    for (unsigned i = 0; i < 3; i++) {
        output_channel_config_t *c = &cfg->channel[i];
        c->enable = i == 0;
        c->crop.enable = i == 2 && (w > 640 || h > 640);
        c->crop.x = c->crop.y = 0;
        c->crop.width = i == 2 && w > 640 ? 640 : w;
        c->crop.height = i == 2 && h > 640 ? 640 : h;
        c->resizer.enable = false;
        c->resizer.preset = 0;
    }
}

isp_status_t output_compute_geometry(const output_channel_config_t *cfg, unsigned channel,
                                     uint16_t w, uint16_t h, output_geometry_t *g)
{
    if (!cfg || !g || channel > 2 || w < 32 || h < 32 || w > 3840 || h > 2160 || ((w|h)&1u))
        return ISP_E_PARAM;
    uint32_t cw = w, ch = h;
    /* Các trường crop vẫn được ghi khi crop tắt; mọi giá trị phải vừa trường thanh ghi. */
    if (cfg->crop.x > 8191 || cfg->crop.y > 8191 ||
        cfg->crop.width > 8191 || cfg->crop.height > 8191) return ISP_E_PARAM;
    if (cfg->crop.enable) {
        const crop_config_t *c = &cfg->crop;
        if (c->width < 2 || c->height < 2 || ((c->x|c->y|c->width|c->height)&1u) ||
            (uint32_t)c->x+c->width > w || (uint32_t)c->y+c->height > h) return ISP_E_PARAM;
        cw = c->width;
        ch = c->height;
    }
    unsigned code = cfg->resizer.preset;
    if (code >= 22 || code == 15 || (!cfg->resizer.enable && code != 0)) return ISP_E_PARAM;
    if (channel == 2 && code && code != 8 && code != 14 && code < 16) return ISP_E_PARAM;
    uint32_t ow = cfg->resizer.enable && code ? targets[code][0] : cw;
    uint32_t oh = cfg->resizer.enable && code ? targets[code][1] : ch;
    if (ow > cw || oh > ch || ow > (channel == 2 ? 640u : 3840u) ||
        oh > (channel == 2 ? 640u : 2160u)) return ISP_E_PARAM;
    g->width = (uint16_t)ow;
    g->height = (uint16_t)oh;
    g->format = channel == 2 ? OUTPUT_RGB888 : OUTPUT_NV12;
    return ISP_OK;
}

isp_status_t output_validate_config(const output_config_t *cfg, uint16_t w, uint16_t h)
{
    if (!cfg || cfg->done_irq > 7 || cfg->error_irq > 7) return ISP_E_PARAM;
    unsigned enabled = 0;
    for (unsigned i = 0; i < 3; i++) {
        output_geometry_t g;
        if (output_compute_geometry(&cfg->channel[i], i, w, h, &g) != ISP_OK) return ISP_E_PARAM;
        if (cfg->channel[i].enable) enabled++;
    }
    return enabled ? ISP_OK : ISP_E_PARAM;
}

isp_status_t output_configure(const output_config_t *cfg, uint16_t w, uint16_t h)
{
    isp_status_t st = output_validate_config(cfg,w,h);
    if (st != ISP_OK) return st;
    isp_reg_setting_t words[14];
    unsigned n = 0, enabled = 0;
    for (unsigned i = 0; i < 3; i++) {
        const output_channel_config_t *c = &cfg->channel[i];
        words[n++] = (isp_reg_setting_t){crop_base[i]+4,0x1fff1fffu,c->crop.x | ((uint32_t)c->crop.y<<16)};
        words[n++] = (isp_reg_setting_t){crop_base[i]+8,0x1fff1fffu,c->crop.width | ((uint32_t)c->crop.height<<16)};
        words[n++] = (isp_reg_setting_t){crop_base[i],1,c->crop.enable ? 1u : 0u};
        words[n++] = (isp_reg_setting_t){resizer_base[i],0xf9u,((uint32_t)c->resizer.preset<<3) | (c->resizer.enable ? 1u : 0u)};
        if (c->enable) enabled |= 1u<<i;
    }
    words[n++] = (isp_reg_setting_t){FX1_ISP_OUTPUT_IRQ_EN_OFFSET,0x77u,cfg->done_irq | ((uint32_t)cfg->error_irq<<4)};
    words[n++] = (isp_reg_setting_t){FX1_ISP_OUTPUT_ENABLE_OFFSET,7u,enabled};
    return isp_configure_registers(words,n);
}

isp_status_t output_wait_geometry(const output_config_t *cfg, uint16_t w, uint16_t h, const isp_wait_t *wait)
{
    isp_status_t st = output_validate_config(cfg,w,h);
    if (st != ISP_OK) return st;
    for (unsigned i = 0; i < 3; i++) if (cfg->channel[i].enable) {
        output_geometry_t g;
        (void)output_compute_geometry(&cfg->channel[i],i,w,h,&g);
        st = isp_wait_register(crop_base[i]+12,3u,1u,wait);
        if (st != ISP_OK) return (REG_RD(crop_base[i]+12)&0xf0u) ? ISP_E_IO : st;
        if ((REG_RD(crop_base[i]+12)&0xf0u) ||
            (REG_RD(resizer_base[i]+0x14)&0x1fffu) != g.width ||
            (REG_RD(resizer_base[i]+0x18)&0x1fffu) != g.height) return ISP_E_IO;
    }
    return ISP_OK;
}
