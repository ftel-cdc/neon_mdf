/* Application tuning; RAW12 input, strided NV12 output in per-frame DDR slots. */
#include "fx1_fw.h"
#include "pipeline/ftel_isp_stream.h"
#include "profile.h"
#ifdef NEON_MDF_HAS_ISP_3A
#include "ftel_isp_auto.h"
extern const unsigned char fpt_sensor_start[], fpt_sensor_end[];
extern const unsigned char fpt_tuning_start[], fpt_tuning_end[];
/* Caller-owned opaque storage. The library checks its queried size/alignment. */
static union {
    uint64_t alignment;
    unsigned char bytes[160u * 1024u];
} auto_memory;
static ftel_isp_auto_t auto_handle;
static ftel_isp_auto_controls_t effective;
#endif
#define WIDTH 2688u
#define HEIGHT 1520u
#define RAW_BASE 0x81000000u
#define OUTPUT_BASE 0x88000000u
#define SLOT_BYTES 0x00800000u
#ifndef FRAME_COUNT
#define FRAME_COUNT 2u
#endif
_Static_assert(WIDTH * HEIGHT * 2u < SLOT_BYTES, "RAW slot capacity");
_Static_assert(RAW_BASE + 4u * SLOT_BYTES <= FX1_FW_YUV_BUF_BASE, "RAW region");
_Static_assert(OUTPUT_BASE + 4u * SLOT_BYTES < FX1_FW_TEST_AREA_BASE, "output region");
static uint64_t time_us(void *unused)
{
    (void)unused;
    return fx1_mtime() / (FX1_MTIME_HZ / 1000000u);
}
static void show(const char *name, uint32_t value)
{
    fx1_puts(name); fx1_puts(" = "); fx1_puthex(value); fx1_puts("\n");
}
int main(void)
{
    fx1_puts("[PASS] FW_BOOT: Neon ISP application profile\n");
    static isp_config_t cfg;
    isp_get_default_config(&cfg);
    cfg.ifmt = (ifmt_config_t){WIDTH, HEIGHT, BAYER_BGGR, INPUT_DEPTH_RAW12};
    output_get_default_config(&cfg.output, WIDTH, HEIGHT);
    apply_profile(&cfg);
#ifdef NEON_MDF_HAS_ISP_3A
    uint32_t auto_flags = 0u;
#if FX1_3A_REPLAY
    auto_flags |= FTEL_ISP_AUTO_REPLAY;
    fx1_puts("[INFO] 3A_REPLAY: sensor mode differs; AE proposals are reference only\n");
#endif
#if FX1_3A_REPLAY_UNSCREENED_AWB
    auto_flags |= FTEL_ISP_AUTO_REPLAY_UNSCREENED_AWB;
    fx1_puts("[INFO] 3A_REPLAY_UNSCREENED_AWB: ROI screening explicitly disabled\n");
#else
    fx1_puts("[INFO] 3A_AWB: hold gains when scalar ROI UE/OE populations are unavailable\n");
#endif
    show("3A_MEMORY_BYTES", ftel_isp_auto_memory_size());
    ftel_isp_status_t auto_status = ftel_isp_auto_init(&auto_handle,
        auto_memory.bytes, sizeof(auto_memory.bytes),
        fpt_sensor_start, (size_t)(fpt_sensor_end - fpt_sensor_start),
        fpt_tuning_start, (size_t)(fpt_tuning_end - fpt_tuning_start),
        auto_flags, &cfg, &effective);
    show("3A_INIT_STATUS", auto_status);
    if (auto_status != FTEL_ISP_OK) return 20;
#endif
    fx1_puts("[INFO] Application tuning via Neon legacy APIs\n");

    show("FRAME_COUNT", FRAME_COUNT);
    output_geometry_t geometry[3];
    for (unsigned c = 0; c < 3; ++c)
        if (output_compute_geometry(&cfg.output.channel[c], c, WIDTH, HEIGHT, &geometry[c]) != ISP_OK)
            return 1;
    dma_get_default_config(&cfg.dma, WIDTH, geometry);
    cfg.dma.input_stride = WIDTH * 2u;
    cfg.dma.output[0].y_stride = cfg.dma.output[0].uv_stride = WIDTH;
    for (unsigned slot = 0; slot < 4; ++slot) {
        cfg.dma.input[slot] = RAW_BASE + slot * SLOT_BYTES;
        cfg.dma.output[0].y[slot] = OUTPUT_BASE + slot * SLOT_BYTES;
        cfg.dma.output[0].uv[slot] = cfg.dma.output[0].y[slot] + 0x00400000u;
    }
    isp_wait_t wait = {time_us, NULL, 2000000, 10000000};
    isp_init_result_t init;
    isp_status_t st = isp_init(&cfg, &wait, &init);
    show("INIT_STATUS", st); show("INIT_STAGE", init.stage);
    if (st != ISP_OK) return 2;
    show("BLC_CTRL", REG_RD(R_BLC_CTRL));
    show("LSC_CTRL", REG_RD(R_LSC_CTRL));
    if ((REG_RD(R_BLC_CTRL) & 1u) != (uint32_t)cfg.blc.enable ||
        (REG_RD(R_LSC_CTRL) & 1u) != (uint32_t)cfg.lsc.enable) return 3;
    const struct { const char *name; uint32_t offset; bool enabled; } controls[] = {
        {"BLC", FX1_ISP_BLC_CTRL_OFFSET, cfg.blc.enable}, {"LSC", FX1_ISP_LSC_CTRL_OFFSET, cfg.lsc.enable},
        {"BPC", FX1_ISP_BPC_CTRL_OFFSET, cfg.bpc.enable}, {"WB", FX1_ISP_WB_CTRL_OFFSET, cfg.wb.enable},
        {"DG", FX1_ISP_DG_CTRL_OFFSET, cfg.dg.enable}, {"CCM", FX1_ISP_CCM_CTRL_OFFSET, cfg.ccm.enable},
        {"GAMMA", FX1_ISP_GAMMA_CTRL_OFFSET, cfg.gamma.enable}, {"GTM", FX1_ISP_GTM_CTRL_OFFSET, cfg.gtm.enable},
        {"NR_2D", FX1_ISP_NR_2D_CTRL_OFFSET, cfg.nr.enable}, {"EE", FX1_ISP_EE_CTRL_OFFSET, cfg.ee.enable},
        {"CNF", FX1_ISP_CNF_CTRL_OFFSET, cfg.cnf.enable}, {"AEC", FX1_ISP_AEC_CTRL_OFFSET, cfg.aec.enable},
        {"AWB", FX1_ISP_AWB_CTRL_OFFSET, cfg.awb.enable}, {"AF", FX1_ISP_AF_CTRL_OFFSET, cfg.af.enable}
    };
    for (unsigned i = 0; i < sizeof controls / sizeof controls[0]; ++i) {
        uint32_t value = REG_RD(controls[i].offset);
        show(controls[i].name, value);
        uint32_t expected = controls[i].enabled;
        if ((value & 1u) != expected) return 10;
    }
    fx1_puts("[PASS] Block enable bits match application profile\n");
    show("PROFILE_WB_R_Q8", REG_RD(FX1_ISP_WB_GAIN_R_OFFSET));
    show("PROFILE_WB_G_Q8", REG_RD(FX1_ISP_WB_GAIN_G_OFFSET));
    show("PROFILE_WB_B_Q8", REG_RD(FX1_ISP_WB_GAIN_B_OFFSET));
    show("PROFILE_DG_Q8", REG_RD(FX1_ISP_DG_GAIN_OFFSET));
    if (REG_RD(FX1_ISP_WB_GAIN_R_OFFSET) != cfg.wb.gain_r ||
        REG_RD(FX1_ISP_WB_GAIN_G_OFFSET) != cfg.wb.gain_g ||
        REG_RD(FX1_ISP_WB_GAIN_B_OFFSET) != cfg.wb.gain_b ||
        REG_RD(FX1_ISP_DG_GAIN_OFFSET) != cfg.dg.gain) return 11;
    for (unsigned i = 0; i < 9; ++i) {
        uint32_t value = REG_RD(FX1_ISP_CCM_CRR_OFFSET + 4u * i) & 0xfffu;
        show("PROFILE_CCM_Q9", value);
        if (value != ((uint32_t)cfg.ccm.matrix[i] & 0xfffu)) return 12;
    }
    fx1_puts("[PASS] WB/DG/CCM profile readback\n");
    fx1_dma_prepare((void *)(uintptr_t)RAW_BASE, WIDTH * HEIGHT * 2u, FX1_DMA_TO_DEVICE);
    fx1_dma_prepare((void *)(uintptr_t)OUTPUT_BASE, SLOT_BYTES, FX1_DMA_FROM_DEVICE);
    isp_stream_t stream = {0};
    st = isp_stream_start(&stream, &cfg, &wait);
    show("START_STATUS", st);
    if (st != ISP_OK) return 4;
    show("OUTPUT_WIDTH", geometry[0].width);
    show("OUTPUT_HEIGHT", geometry[0].height);
    for (unsigned index = 0; index < FRAME_COUNT; ++index) {
        unsigned slot = index & 3u;
        fx1_dma_prepare((void *)(uintptr_t)(RAW_BASE + slot * SLOT_BYTES), WIDTH * HEIGHT * 2u, FX1_DMA_TO_DEVICE);
        fx1_dma_prepare((void *)(uintptr_t)(OUTPUT_BASE + slot * SLOT_BYTES), SLOT_BYTES, FX1_DMA_FROM_DEVICE);
        st = isp_stream_submit(&stream, 0x1000u + index, 1000u + index);
        if (st != ISP_OK) return 5;
        isp_frame_result_t frame;
        st = isp_stream_wait(&stream, &wait, &frame);
        show("WAIT_STATUS", st);
        show("FRAME_SLOT", frame.slot); show("DMA_ERRORS", frame.dma.errors);
        show("INPUT_COUNT", frame.dma.input_count); show("OUTPUT_COUNT", frame.dma.output_count[0]);
        show("OUTPUT_TAG", frame.tag[0]);
        if (st != ISP_OK || frame.slot != slot ||
            (frame.dma.errors & ~DMA_ERROR_INPUT_PAUSED) ||
            frame.dma.input_count != index + 1 || frame.dma.output_count[0] != index + 1 ||
            frame.tag[0] != 0x1000u + index) return 6;
        uintptr_t completed_output = REG_RD(FX1_ISP_ODMA_Y_ADDR_L0_OFFSET + 16u * slot);
        fx1_dma_complete((void *)completed_output, SLOT_BYTES, FX1_DMA_FROM_DEVICE);
#ifdef NEON_MDF_HAS_ISP_3A
        ftel_isp_auto_result_t auto_result;
        uint64_t stats_begin = time_us(NULL);
        for (uint32_t poll = 0; poll < 1000000u; ++poll) {
            auto_status = ftel_isp_auto_process(&auto_handle, &effective, &auto_result);
            if (auto_status != FTEL_ISP_E_NO_DATA || time_us(NULL) - stats_begin >= wait.timeout_us) break;
        }
        show("3A_PROCESS_STATUS", auto_status);
        if (auto_status != FTEL_ISP_OK) return 21;
        show("3A_FRAME_ID", auto_result.frame_id);
        show("3A_CONTEXT_ID", auto_result.context_id);
        show("3A_FLAGS", auto_result.flags);
        show("3A_LUMA", auto_result.current_luma);
        show("3A_EFFECTIVE_EXPOSURE_LINES", effective.exposure_lines);
        show("3A_EFFECTIVE_ANALOG_GAIN_Q8", effective.analog_gain_q8);
        show("3A_PROPOSED_EXPOSURE_LINES", auto_result.proposed.exposure_lines);
        show("3A_PROPOSED_ANALOG_GAIN_Q8", auto_result.proposed.analog_gain_q8);
        show("3A_PROPOSED_WB_R_Q8", auto_result.proposed.wb_red_q8);
        show("3A_PROPOSED_WB_B_Q8", auto_result.proposed.wb_blue_q8);
        show("3A_AWB_VALID_ROIS", auto_result.awb_valid_rois);
        show("3A_LUX_Q16", (uint32_t)auto_result.lux_index_q16_16);
        show("3A_CCT", auto_result.cct_kelvin);
#endif
        if (isp_stream_release(&stream) != ISP_OK) return 7;
#ifdef NEON_MDF_HAS_ISP_3A
        if (index + 1u < FRAME_COUNT) {
            auto_status = ftel_isp_auto_apply_wb(&auto_handle, &auto_result, &effective);
            show("3A_APPLY_WB_STATUS", auto_status);
            if (auto_status != FTEL_ISP_OK) return 22;
        }
#endif
    }
    if (isp_stream_stop(&stream, 100000) != ISP_OK) return 8;
    if ((REG_RD(R_BLC_CTRL) & 1u) != (uint32_t)cfg.blc.enable ||
        (REG_RD(R_LSC_CTRL) & 1u) != (uint32_t)cfg.lsc.enable || REG_RD(R_DMA_ERR)) return 9;
    show("NV12_BASE", REG_RD(FX1_ISP_ODMA_Y_ADDR_L0_OFFSET + 16u * ((FRAME_COUNT - 1u) & 3u)));
    show("NV12_BYTES", (uint32_t)geometry[0].width * geometry[0].height * 3u / 2u);
#ifdef NEON_MDF_HAS_ISP_3A
    ftel_isp_auto_reset(&auto_handle);
    fx1_puts("3A_RESULT=PASS\n");
#endif
    fx1_puts("IMAGE_RESULT=PASS\n");
    return 0;
}
