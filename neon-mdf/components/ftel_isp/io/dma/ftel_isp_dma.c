/*
 * Cấu hình DMA và thao tác quyền buffer; chưa có ISR hay phục hồi tự động.
 * Hiện thực và luồng xử lý.
 */
#include "io/dma/ftel_isp_dma.h"
#include "registers/ftel_isp_regs.h"
static uint32_t aligned(uint32_t n, uint32_t alignment) { return (n+alignment-1u)&~(alignment-1u); }
void dma_get_default_config(dma_config_t *cfg, uint16_t input_w, const output_geometry_t g[3])
{
    if (!cfg || !g) return;
    cfg->beat_bytes = 16;
    cfg->address_bits = 40;
    cfg->max_burst_beats = 64;
    cfg->irq_enable = 0;
    cfg->input_stride = aligned(2u*input_w,16);
    for (unsigned i = 0; i < 4; i++) cfg->input[i] = 0;
    for (unsigned c = 0; c < 3; c++) {
        cfg->output[c].y_stride = aligned((c == 2 ? 3u : 1u)*g[c].width,16);
        cfg->output[c].uv_stride = c == 2 ? 0 : aligned(g[c].width,16);
        for (unsigned i = 0; i < 4; i++) cfg->output[c].y[i] = cfg->output[c].uv[i] = 0;
    }
}

static bool fits(uint64_t base, uint32_t stride, uint32_t row, uint16_t h,
                 uint8_t beat, uint8_t bits)
{
    uint64_t limit = UINT64_C(1)<<bits;
    uint64_t span = (uint64_t)(h-1u)*stride+row;
    return !(base&(beat-1u)) && !(stride&(beat-1u)) && stride >= row &&
           base < limit && span <= limit-base;
}

isp_status_t dma_validate_config(const dma_config_t *cfg, uint16_t w, uint16_t h, const output_geometry_t g[3])
{
    if (!cfg || !g || w < 32 || h < 32 || w > 3840 || h > 2160 || ((w|h)&1u) ||
        (cfg->beat_bytes != 8 && cfg->beat_bytes != 16 && cfg->beat_bytes != 32) ||
        (cfg->address_bits != 32 && cfg->address_bits != 40) ||
        !cfg->max_burst_beats || cfg->max_burst_beats > 256 ||
        (cfg->irq_enable & ~FX1_ISP_DMA_IRQ_EN_RW_MASK)) return ISP_E_PARAM;
    for (unsigned i = 0; i < 4; i++)
        if (!fits(cfg->input[i],cfg->input_stride,2u*w,h,cfg->beat_bytes,cfg->address_bits)) return ISP_E_PARAM;
    for (unsigned c = 0; c < 3; c++) {
        if (g[c].width < 2 || g[c].height < 2 || ((g[c].width|g[c].height)&1u) ||
            g[c].width > (c == 2 ? 640u : 3840u) ||
            g[c].height > (c == 2 ? 640u : 2160u) ||
            g[c].format != (c == 2 ? OUTPUT_RGB888 : OUTPUT_NV12)) return ISP_E_PARAM;
        for (unsigned i = 0; i < 4; i++) {
            if (!fits(cfg->output[c].y[i],cfg->output[c].y_stride,(c == 2 ? 3u : 1u)*g[c].width,
                      g[c].height,cfg->beat_bytes,cfg->address_bits)) return ISP_E_PARAM;
            if (c < 2 && !fits(cfg->output[c].uv[i],cfg->output[c].uv_stride,g[c].width,
                              g[c].height/2u,cfg->beat_bytes,cfg->address_bits)) return ISP_E_PARAM;
            if (c == 2 && (cfg->output[c].uv_stride || cfg->output[c].uv[i])) return ISP_E_PARAM;
        }
    }
    return ISP_OK;
}

static isp_status_t write_address(uint32_t lo, uint64_t value)
{
    isp_status_t st = isp_write_checked(lo,0xffffffffu,(uint32_t)value);
    return st == ISP_OK ? isp_write_checked(lo+4,0xffffffffu,(uint32_t)(value>>32)) : st;
}

isp_status_t dma_configure(const dma_config_t *cfg, uint16_t w, uint16_t h, const output_geometry_t g[3])
{
    isp_status_t st = dma_validate_config(cfg,w,h,g);
    if (st != ISP_OK) return st;
    st = isp_require_legacy();
    if (st != ISP_OK) return st;
    if (isp_not_idle() || (REG_RD(0x3088)&15u) || (REG_RD(0x3288)&15u) || (REG_RD(0x3488)&15u)) return ISP_E_NOT_IDLE;
    st = isp_prepare_config();
    if (st != ISP_OK) return st;
    REG_WR(R_DMA_CTRL, ((uint32_t)cfg->max_burst_beats-1u)<<8);
    isp_barrier();
    if (!isp_reg_matches(R_DMA_CTRL,0xff03u,((uint32_t)cfg->max_burst_beats-1u)<<8)) return ISP_E_IO;
    st = isp_write_checked(R_DMA_IRQ_EN,FX1_ISP_DMA_IRQ_EN_RW_MASK,cfg->irq_enable);
    if (st != ISP_OK) return st;
    st = isp_write_checked(FX1_ISP_IDMA_STRIDE_OFFSET,0xffffffffu,cfg->input_stride);
    if (st != ISP_OK) return st;
    for (unsigned i = 0; i < 4; i++) {
        st = write_address(0x3040u+8u*i,cfg->input[i]);
        if (st != ISP_OK) return st;
    }
    for (unsigned c = 0; c < 3; c++) {
        uint32_t page = c == 0 ? 0x3000u : c == 1 ? 0x3200u : 0x3400u;
        st = isp_write_checked(page+0x80,0xffffffffu,cfg->output[c].y_stride);
        if (st != ISP_OK) return st;
        if (c < 2) { st = isp_write_checked(page+0x84,0xffffffffu,cfg->output[c].uv_stride);
            if (st != ISP_OK) return st; }
        for (unsigned i = 0; i < 4; i++) {
            st = write_address(page+0xa0u+16u*i,cfg->output[c].y[i]);
            if (st != ISP_OK) return st;
            if (c < 2) { st = write_address(page+0xa8u+16u*i,cfg->output[c].uv[i]);
                if (st != ISP_OK) return st; }
        }
    }
    if (REG_RD(R_DMA_ERR)) return ISP_E_IO;
    return isp_config_status();
}

static const uint32_t output_free[3] = {
    FX1_ISP_ODMA_BUF_FREE_OFFSET, FX1_ISP_CH1_ODMA_BUF_FREE_OFFSET,
    FX1_ISP_CH2_ODMA_BUF_FREE_OFFSET
};

void dma_read_frame_status(dma_frame_status_t *s)
{
    if (!s) return;
    s->input_valid = REG_RD(R_IDMA_BUF_VALID) & DMA_SLOT_MASK;
    s->input_count = REG_RD(FX1_ISP_IDMA_FRAME_COUNT_OFFSET);
    for (unsigned c = 0; c < 3; c++) {
        s->output_free[c] = REG_RD(output_free[c]) & DMA_SLOT_MASK;
        s->output_done[c] = REG_RD(output_free[c] + 4u) & DMA_SLOT_MASK;
        s->output_count[c] = REG_RD(output_free[c] + 8u);
    }
    s->busy = REG_RD(R_DMA_STAT) & DMA_STAT_BUSY_MASK;
    s->errors = REG_RD(R_DMA_ERR) & FX1_ISP_DMA_ERR_ERR_MASK;
    s->irq = REG_RD(FX1_ISP_DMA_IRQ_STAT_OFFSET);
    s->common_irq = REG_RD(R_COMMON_IRQ_STATUS);
    s->output_irq = REG_RD(FX1_ISP_OUTPUT_IRQ_STATUS_OFFSET);
    s->recovery_status = REG_RD(R_COMMON_RECOVERY_STATUS);
    s->recovery_sequence = REG_RD(R_COMMON_RECOVERY_SEQUENCE);
}

isp_status_t dma_submit_slot(uint8_t slot, uint8_t outputs, uint32_t tag, uint32_t timestamp)
{
    if (slot >= 4 || !outputs || (outputs & ~7u)) return ISP_E_PARAM;
    isp_status_t st = isp_require_legacy();
    if (st != ISP_OK) return st;
    if (!(REG_RD(R_COMMON_CTRL) & COMMON_CTRL_ISP_EN) ||
        (REG_RD(FX1_ISP_OUTPUT_ENABLE_OFFSET) & 7u) != outputs) return ISP_E_STATE;
    if (isp_not_idle() || (REG_RD(R_DMA_CTRL) & DMA_CTRL_ENABLE_MASK)) return ISP_E_NOT_IDLE;
    for (unsigned c = 0; c < 3; c++)
        if ((REG_RD(output_free[c]) | REG_RD(output_free[c] + 4u)) & DMA_SLOT_MASK)
            return ISP_E_NOT_IDLE;
    if (REG_RD(R_DMA_ERR) || isp_tune_error_set()) return ISP_E_IO;

    st = isp_write_checked(FX1_ISP_IDMA_BUF_TAG0_OFFSET + 4u * slot, UINT32_MAX, tag);
    if (st != ISP_OK) return st;
    st = isp_write_checked(FX1_ISP_IDMA_BUF_TS0_OFFSET + 4u * slot, UINT32_MAX, timestamp);
    if (st != ISP_OK) return st;
    uint32_t bit = 1u << slot;
    /* Nền tảng phải clean cache trước lời gọi này nếu DDR không coherent. VP hiện không có D-cache. */
    isp_memory_barrier();
    for (unsigned c = 0; c < 3; c++) if (outputs & (1u << c)) REG_WR(output_free[c], bit);
    isp_barrier();
    REG_WR(R_IDMA_BUF_VALID, bit);
    isp_barrier();
    if (!(REG_RD(R_IDMA_BUF_VALID) & bit)) return ISP_E_IO;
    for (unsigned c = 0; c < 3; c++)
        if ((outputs & (1u << c)) && !(REG_RD(output_free[c]) & bit)) return ISP_E_IO;
    uint32_t control = (REG_RD(R_DMA_CTRL) & DMA_CTRL_BURST_MASK) | DMA_CTRL_ENABLE_MASK;
    REG_WR(R_DMA_CTRL, control);
    isp_barrier();
    return isp_reg_matches(R_DMA_CTRL, DMA_CTRL_BURST_MASK | DMA_CTRL_ENABLE_MASK, control)
        && !isp_tune_error_set() ? ISP_OK : ISP_E_IO;
}

isp_status_t dma_pause(void)
{
    if (isp_not_idle()) return ISP_E_NOT_IDLE;
    uint32_t control = REG_RD(R_DMA_CTRL) & DMA_CTRL_BURST_MASK;
    REG_WR(R_DMA_CTRL, control);
    isp_barrier();
    return isp_reg_matches(R_DMA_CTRL, DMA_CTRL_BURST_MASK | DMA_CTRL_ENABLE_MASK, control)
        ? ISP_OK : ISP_E_IO;
}

isp_status_t dma_release_slot(uint8_t slot, uint8_t outputs)
{
    if (slot >= 4 || !outputs || (outputs & ~7u)) return ISP_E_PARAM;
    if (isp_not_idle() || (REG_RD(R_DMA_CTRL) & DMA_CTRL_ENABLE_MASK)) return ISP_E_NOT_IDLE;
    uint32_t bit = 1u << slot;
    for (unsigned c = 0; c < 3; c++) if (outputs & (1u << c)) {
        /* Hardware set thắng W1C cùng chu kỳ; kiểm tra lại và thử ở giao dịch kế tiếp. */
        for (unsigned attempt = 0; attempt < 2; attempt++) {
            REG_WR(output_free[c] + 4u, bit);
            isp_barrier();
            if (!(REG_RD(output_free[c] + 4u) & bit)) break;
        }
        if (REG_RD(output_free[c] + 4u) & bit) return ISP_E_IO;
    }
    return isp_tune_error_set() ? ISP_E_IO : ISP_OK;
}
