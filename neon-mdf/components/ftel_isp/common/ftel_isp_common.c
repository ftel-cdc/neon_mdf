/*
 * Điều khiển ISP dùng chung: ABI, trạng thái rỗi, hàng rào MMIO và chờ có giới hạn.
 * Hiện thực và luồng xử lý.
 */
#include "common/ftel_isp_common.h"
#include "registers/ftel_isp_regs.h"

void isp_barrier(void)
{
    isp_memory_barrier();
}

void isp_memory_barrier(void)
{
#if defined(__arm__) || defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#elif defined(__riscv)
    __asm__ volatile("fence iorw, iorw" ::: "memory");
#else
    __asm__ volatile("" ::: "memory");
#endif
}

isp_status_t isp_check_abi(void)
{
    return (REG_RD(R_COMMON_VER_ID) == ISP_ABI_VERSION)
           ? ISP_OK : ISP_E_BAD_ABI;
}

bool isp_pipeline_busy(void)
{
    return (REG_RD(R_COMMON_STATUS) & COMMON_STATUS_BUSY) != 0u;
}

bool isp_not_idle(void)
{
    if (isp_pipeline_busy())
        return true;
#if ISP_CHECK_RECOVERY_IDLE
    if (REG_RD(R_COMMON_RECOVERY_STATUS) & COMMON_RECOVERY_STATUS_BUSY)
        return true;
#endif
    if (REG_RD(R_DMA_STAT) & DMA_STAT_BUSY_MASK) return true;
    if (REG_RD(R_IDMA_BUF_VALID) & IDMA_BUF_VALID_MASK) return true;
    return false;
}

isp_status_t isp_stop_and_wait(uint32_t timeout)
{
    if (!timeout) return ISP_E_PARAM;
    isp_status_t st = isp_check_abi();
    if (st != ISP_OK) return st;
    while (isp_not_idle()) {
        if (timeout-- == 0u)
            return ISP_E_TIMEOUT;
    }
    REG_WR(R_DMA_CTRL, REG_RD(R_DMA_CTRL) & DMA_CTRL_BURST_MASK);
    isp_barrier();
    return isp_clear_common_ctrl();
}

isp_status_t isp_tune_error_clear(void)
{
    if (REG_RD(R_TUNE_ID) != TUNE_ID_MAGIC)
        return ISP_E_BAD_ABI;

    /* TUNE_ERROR là thanh ghi W1C: ghi 1 vào từng bit để xóa các lỗi đã chốt. */
    REG_WR(R_TUNE_ERROR, TUNE_ERROR_STICKY_MASK);
    isp_barrier();
    return ISP_OK;
}

bool isp_tune_error_set(void)
{
    return (REG_RD(R_TUNE_ERROR) & TUNE_ERROR_STICKY_MASK) != 0u;
}

isp_status_t isp_clear_common_ctrl(void)
{
    /*
     * COMMON_CTRL trộn ISP_EN (RW), SOFT_RST (W1SC) và FRAME_START (W1S), nên không đọc-sửa-ghi cả
     * thanh ghi. Ghi 0 để tắt pipeline mà không phát lại lệnh xung.
     */
    REG_WR(R_COMMON_CTRL, 0u);
    isp_barrier();
    return isp_reg_matches(R_COMMON_CTRL, COMMON_CTRL_ISP_EN, 0u) ? ISP_OK : ISP_E_IO;
}

isp_status_t isp_enable_pipeline(void)
{
    REG_WR(R_COMMON_CTRL, COMMON_CTRL_ISP_EN);
    isp_barrier();
    return isp_reg_matches(R_COMMON_CTRL, COMMON_CTRL_ISP_EN, COMMON_CTRL_ISP_EN) ? ISP_OK : ISP_E_IO;
}

bool isp_in_profile_mode(void)
{
    return (REG_RD(R_TUNE_MODE) & TUNE_MODE_ENABLE) != 0u;
}

isp_status_t isp_check_tune_cap(uint32_t cap_bit)
{
    isp_status_t st = isp_check_abi();
    if (st != ISP_OK) return st;
    if (REG_RD(R_TUNE_ID) != TUNE_ID_MAGIC)
        return ISP_E_BAD_ABI;
    if ((REG_RD(R_TUNE_CAPS) & cap_bit) != cap_bit)
        return ISP_E_BAD_ABI;
    return ISP_OK;
}

isp_status_t isp_require_legacy(void)
{
    isp_status_t st = isp_check_abi();
    if (st != ISP_OK) return st;
    if (REG_RD(R_TUNE_ID) != TUNE_ID_MAGIC) return ISP_E_BAD_ABI;
    return isp_in_profile_mode() ? ISP_E_NOT_IDLE : ISP_OK;
}

isp_status_t isp_prepare_config(void)
{
    isp_status_t st = isp_require_legacy();
    return st == ISP_OK ? isp_tune_error_clear() : st;
}

isp_status_t isp_config_status(void)
{
    isp_barrier();
    uint32_t error = REG_RD(R_TUNE_ERROR);
    if (error & TUNE_ERROR_MODE) return ISP_E_NOT_IDLE;
    return error ? ISP_E_IO : ISP_OK;
}

bool isp_reg_matches(uint32_t address, uint32_t mask, uint32_t value)
{
    return (REG_RD(address) & mask) == (value & mask);
}

isp_status_t isp_write_checked(uint32_t address, uint32_t mask, uint32_t value)
{
    REG_UPD(address, mask, value);
    isp_barrier();
    isp_status_t st = isp_config_status();
    if (st != ISP_OK) return st;
    return isp_reg_matches(address, mask, value) ? ISP_OK : ISP_E_IO;
}

void isp_get_default_wait(isp_wait_t *wait)
{
    if (!wait) return;
    wait->time_us = 0;
    wait->user = 0;
    wait->timeout_us = 1000;
    wait->poll_limit = ISP_IDLE_DRAIN_TIMEOUT;
}

isp_status_t isp_configure_registers(const isp_reg_setting_t *list, size_t count)
{
    if (!list || !count) return ISP_E_PARAM;
    for (size_t i = 0; i < count; i++)
        if (list[i].value & ~list[i].mask) return ISP_E_PARAM;
    isp_status_t st = isp_require_legacy();
    if (st != ISP_OK) return st;
    if (isp_not_idle()) return ISP_E_NOT_IDLE;
    st = isp_prepare_config();
    if (st != ISP_OK) return st;
    for (size_t i = 0; i < count; i++) {
        st = isp_write_checked(list[i].address, list[i].mask, list[i].value);
        if (st != ISP_OK) return st;
    }
    return ISP_OK;
}

isp_status_t isp_wait_register(uint32_t address, uint32_t mask, uint32_t value,
                               const isp_wait_t *wait)
{
    isp_wait_t fallback;
    if (!wait) { isp_get_default_wait(&fallback);
        wait = &fallback; }
    if (!wait->poll_limit || (wait->time_us && !wait->timeout_us)) return ISP_E_PARAM;
    uint64_t start = wait->time_us ? wait->time_us(wait->user) : 0;
    for (uint32_t n = 0; n < wait->poll_limit; n++) {
        if (isp_reg_matches(address, mask, value)) return ISP_OK;
        if (wait->time_us && wait->time_us(wait->user) - start >= wait->timeout_us)
            break;
    }
    return ISP_E_TIMEOUT;
}
