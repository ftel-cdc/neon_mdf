/* Một task gọi HAL của module ftel_isp; chỉ kiểm chứng init/readback. */
#include "ftel_isp.h"
#include "mcu_device.h"
#include "neon_freertos_fx1.h"
#include "task.h"
#include "registers/ftel_isp_regs.h" /* Chỉ bài readback mới dùng regmap private. */

static unsigned checks;

static uint32_t read_csr(uint32_t offset)
{
    return fx1_read32(FX1_ISP_CSR_BASE + offset);
}

static void check(bool ok, const char *name)
{
    ++checks;
    fx1_puts(ok ? "[ftel_isp] PASS " : "[ftel_isp] FAIL ");
    fx1_puts(name);
    fx1_putc('\n');
    if (!ok) fx1_fail(0xe800u + checks);
}

static void isp_init_task(void *arg)
{
    (void)arg;
    ftel_isp_config_t config;
    ftel_isp_init_result_t result;
    uint32_t width_before = read_csr(FX1_ISP_COMMON_FRAME_WIDTH_OFFSET);
    ftel_isp_get_default_config(&config);
    check(read_csr(FX1_ISP_COMMON_FRAME_WIDTH_OFFSET) == width_before,
          "get_default only fills RAM");

    /* Giá trị bring-up dễ đọc lại; chưa phải tuning cho ảnh/sensor. */
    config.bayer.blc.enable = true;
    config.bayer.blc.black_level = 64;
    config.bayer.wb.enable = true;
    config.bayer.wb.r_q8 = 384;
    config.bayer.wb.b_q8 = 512;
    config.bayer.dg.enable = true;
    config.bayer.dg.gain_q8 = 384;
    config.aec.context_id = 0x46583101u;

    ftel_isp_status_t status = ftel_isp_init_bayer_aec(&config, &result);
    fx1_log_hex("ftel_isp init status", (uint32_t)status);
    fx1_log_hex("ftel_isp init stage", (uint32_t)result.stage);
    fx1_log_hex("ftel_isp driver status", (uint32_t)result.driver_status);
    check(status == FTEL_ISP_OK && result.stage == FTEL_ISP_INIT_READY,
          "public API -> pipeline -> Bayer/AEC init");
    check(read_csr(FX1_ISP_COMMON_VER_ID_OFFSET) == FX1_ISP_COMMON_VER_ID_RESET,
          "ISP ABI v1.07");
    check(result.active_width == 64 && result.active_height == 48,
          "active geometry from IFMT");
    check(read_csr(FX1_ISP_COMMON_FRAME_WIDTH_OFFSET) == 64 &&
          read_csr(FX1_ISP_COMMON_FRAME_HEIGHT_OFFSET) == 48 &&
          read_csr(FX1_ISP_COMMON_INPUT_FORMAT_OFFSET) == 0 &&
          read_csr(FX1_ISP_COMMON_BAYER_OFFSET) == 0, "IFMT RAW12/RGGB 64x48");
    check((read_csr(FX1_ISP_BLC_CTRL_OFFSET) & 1u) == 1u &&
          read_csr(FX1_ISP_BLC_OFS_G0_DFT_OFFSET) == 64, "BLC enable and black level");
    check((read_csr(FX1_ISP_LSC_CTRL_OFFSET) & 1u) == 0, "LSC bypass, no table load");
    check((read_csr(FX1_ISP_BPC_CTRL_OFFSET) & 1u) == 0, "BPC bypass");
    check((read_csr(FX1_ISP_WB_CTRL_OFFSET) & 1u) == 1u &&
          read_csr(FX1_ISP_WB_GAIN_R_OFFSET) == 384 &&
          read_csr(FX1_ISP_WB_GAIN_G_OFFSET) == 256 &&
          read_csr(FX1_ISP_WB_GAIN_B_OFFSET) == 512, "WB enable and gains");
    check((read_csr(FX1_ISP_DG_CTRL_OFFSET) & 1u) == 1u &&
          read_csr(FX1_ISP_DG_GAIN_OFFSET) == 384, "DG enable and gain");
    check((read_csr(FX1_ISP_AEC_CTRL_OFFSET) & 1u) == 1u &&
          read_csr(FX1_ISP_AEC_CONTEXT_ID_OFFSET) == config.aec.context_id &&
          read_csr(FX1_ISP_TUNE_AEC_SOURCE_OFFSET) == 1, "AEC enable/source/context");
    check((read_csr(FX1_ISP_COMMON_CTRL_OFFSET) & 1u) == 0 &&
          (read_csr(FX1_ISP_DMA_CTRL_OFFSET) & 3u) == 0,
          "ISP admission and DMA remain disabled");

    ftel_isp_info_t info;
    check(ftel_isp_get_info(&info) == FTEL_ISP_OK &&
          info.date == FX1_ISP_COMMON_VER_DATE_RESET &&
          info.tune_revision == FX1_ISP_TUNE_REV_RESET && !info.busy,
          "HAL reports version/date/revision and idle state");
    /* Admission bật nhưng chưa có frame cũng phải được init coi là đang dùng. */
    fx1_write32(FX1_ISP_CSR_BASE + FX1_ISP_COMMON_CTRL_OFFSET,
                FX1_ISP_COMMON_CTRL_ISP_EN_MASK);
    uint32_t sequence = read_csr(FX1_ISP_COMMON_RECOVERY_SEQUENCE_OFFSET);
    check(ftel_isp_init_bayer_aec(&config, &result) == FTEL_ISP_E_BUSY &&
          result.stage == FTEL_ISP_INIT_IDLE &&
          read_csr(FX1_ISP_COMMON_RECOVERY_SEQUENCE_OFFSET) == sequence,
          "init rejects enabled ISP before reset");
    fx1_write32(FX1_ISP_CSR_BASE + FX1_ISP_COMMON_CTRL_OFFSET, 0);

    /* Cấu hình sai phải dừng ở VALIDATE, không làm đổi cấu hình đã ghi. */
    config.bayer.input.width = 65;
    check(ftel_isp_init_bayer_aec(&config, &result) == FTEL_ISP_E_CONFIG &&
          result.stage == FTEL_ISP_INIT_VALIDATE &&
          read_csr(FX1_ISP_COMMON_FRAME_WIDTH_OFFSET) == 64, "invalid input rejected before MMIO");
    config.bayer.input.width = 48;
    config.bayer.dg.gain_q8 = 0xffffu;
    check(ftel_isp_init_bayer_aec(&config, &result) == FTEL_ISP_E_CONFIG &&
          result.stage == FTEL_ISP_INIT_VALIDATE &&
          read_csr(FX1_ISP_COMMON_FRAME_WIDTH_OFFSET) == 64, "invalid DG rejected before MMIO");
    fx1_puts("[ftel_isp] BAYER_AEC_INIT PASS checks=");
    fx1_putdec(checks);
    fx1_putc('\n');
    fx1_pass();
}

int main(void)
{
    fx1_log("MCU FX1 / module ftel_isp / Bayer -> AEC init");
    NEON_FX1_FreeRTOS_Init();
    configASSERT(xTaskCreate(isp_init_task, "isp_init", 1024, NULL, 2, NULL) == pdPASS);
    vTaskStartScheduler();
    fx1_fail(0xe8ff);
}
