/* VP test only. Supply prepared UE/OE as if the hardware stats reader had
 * returned them. Not linked into the normal firmware or SDK component.
 * The host loads: magic 'UEO1', ROI count, then {sample_count, UE, OE} u32 LE.
 */
#include "ftel_isp_auto_private.h"
#include "fx1_fw.h"
#include "pipeline/ftel_isp_stream.h"

static void print_sum(uint64_t value) {
    char digits[20];
    unsigned count = 0;
    do {
        digits[count++] = '0' + value % 10u;
        value /= 10u;
    } while (value);
    while (count) {
        fx1_putc(digits[--count]);
    }
}

#ifdef VP_CAPTURE_FRAME_COUNT
/* VP test capture only: preserve every CH0 output in a separate DDR buffer.
 * Four hardware descriptors still rotate normally. No pixel copying or new
 * path in the SDK; the VP has no D-cache. The host dumps these after PASS.
 */
_Static_assert(0x90800000ull + (uint64_t)VP_CAPTURE_FRAME_COUNT * 0x800000u <=
    (uint64_t)FX1_DDR_BASE + FX1_DDR_SIZE, "VP frame archive exceeds DDR");
isp_status_t __real_isp_stream_submit(isp_stream_t *stream, uint32_t tag, uint32_t timestamp);
isp_status_t __wrap_isp_stream_submit(isp_stream_t *stream, uint32_t tag, uint32_t timestamp) {
    if (!stream || stream->state != ISP_STREAM_READY || stream->outputs != 1u ||
        stream->sequence >= VP_CAPTURE_FRAME_COUNT) {
        return ISP_E_STATE;
    }
    uint32_t address = 0x90800000u + stream->sequence * 0x800000u;
    uint32_t offset = FX1_ISP_ODMA_Y_ADDR_L0_OFFSET + 16u * stream->next_slot;
    const isp_reg_setting_t settings[] = {
        {offset, UINT32_MAX, address}, {offset + 4u, UINT32_MAX, 0u},
        {offset + 8u, UINT32_MAX, address + 0x400000u}, {offset + 12u, UINT32_MAX, 0u}
    };
    isp_status_t status = isp_configure_registers(settings, 4);
    if (status == ISP_OK) {
        fx1_dma_prepare((void *)(uintptr_t)address, 0x800000u, FX1_DMA_FROM_DEVICE);
        fx1_puts("VP_OUTPUT,"); fx1_putdec(stream->sequence + 1u); fx1_putc(',');
        fx1_puthex(address); fx1_putc('\n');
    }
    return status == ISP_OK ? __real_isp_stream_submit(stream, tag, timestamp) : status;
}
#endif

ftel_isp_status_t __real_ftel_isp_auto_collect(ftel_auto_workspace_t *workspace);

ftel_isp_status_t __wrap_ftel_isp_auto_collect(ftel_auto_workspace_t *workspace) {
    ftel_isp_status_t status = __real_ftel_isp_auto_collect(workspace);
    if (status != FTEL_ISP_OK) {
        return status;
    }
    const uint32_t *counts = (const uint32_t *)(uintptr_t)FX1_FW_TEST_AREA_BASE;
    if (counts[0] != 0x314f4555u || counts[1] != workspace->stats.awb_zone_count) {
        return FTEL_ISP_E_CONFIG;
    }
    for (uint32_t i = 0; i < counts[1]; ++i) {
        uint32_t offset = 2u + i * 3u;
        uint32_t samples = counts[offset], ue = counts[offset + 1u], oe = counts[offset + 2u];
        if (ue > samples || oe > samples - ue) {
            return FTEL_ISP_E_CONFIG;
        }
        workspace->awb[i].sample_count = samples;
        workspace->awb[i].ue_count = ue;
        workspace->awb[i].oe_count = oe;
    }
    workspace->stats.awb_populations_valid = 1u;
    uint64_t r = 0u, g = 0u, b = 0u;
    for (uint32_t i = 0; i < workspace->stats.awb_zone_count; ++i) {
        r += workspace->awb[i].red_sum;
        g += workspace->awb[i].green_sum;
        b += workspace->awb[i].blue_sum;
    }
    /* Global pre-WB ratios are computed on the host from these exact sums.
     * Applied gains belong to this measured frame, not the next proposal. */
    fx1_puts("AWB_RAW,"); fx1_putdec(workspace->stats.frame_id); fx1_putc(',');
    print_sum(r); fx1_putc(','); print_sum(g); fx1_putc(','); print_sum(b); fx1_putc(',');
    fx1_putdec(REG_RD(FX1_ISP_WB_GAIN_R_OFFSET)); fx1_putc(',');
    fx1_putdec(REG_RD(FX1_ISP_WB_GAIN_B_OFFSET)); fx1_putc('\n');
    return FTEL_ISP_OK;
}
