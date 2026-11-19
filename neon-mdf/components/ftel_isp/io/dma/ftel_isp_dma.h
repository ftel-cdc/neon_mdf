/*
 * Cấu hình DMA và thao tác quyền buffer. dma_configure() chưa cấp quyền chạy ảnh.
 * Khai báo API và cấu trúc dữ liệu.
 */
#ifndef FTEL_ISP_DMA_H
#define FTEL_ISP_DMA_H
#include "io/output/ftel_isp_output.h"

typedef struct {
    uint32_t y_stride, uv_stride;
    uint64_t y[4], uv[4];
} dma_output_config_t;

typedef struct {
    uint8_t beat_bytes, address_bits; /* Tham số lúc xây dựng IP: số byte mỗi nhịp 8/16/32 và độ rộng địa chỉ 32/40 bit. */
    uint16_t max_burst_beats;         /* 1..256 nhịp; mã ghi thanh ghi = số nhịp - 1. */
    uint32_t irq_enable, input_stride;
    uint64_t input[4];
    dma_output_config_t output[3];
} dma_config_t;

/* Địa chỉ 0 chỉ là chỗ giữ chưa cấp quyền; init không đánh dấu VALID/FREE để DMA sử dụng. */
void dma_get_default_config(dma_config_t *cfg, uint16_t input_w, const output_geometry_t geometry[3]);
isp_status_t dma_validate_config(const dma_config_t *cfg, uint16_t input_w, uint16_t input_h,
                                 const output_geometry_t geometry[3]);
isp_status_t dma_configure(const dma_config_t *cfg, uint16_t input_w, uint16_t input_h,
                           const output_geometry_t geometry[3]);

/* Bốn slot thật của FX1; các bit 7:4 trong regmap là phần dành cho build khác. */
#define DMA_SLOT_MASK FX1_ISP_IDMA_BUF_VALID_IDMA_BUF_VALID_MASK
#define DMA_ERROR_INPUT_PAUSED FX1_ISP_DMA_ERR_ERR_IDMA_UNDERRUN_BIT
typedef struct {
    uint32_t input_valid, input_count;
    uint32_t output_free[3], output_done[3], output_count[3];
    uint32_t busy, errors, irq, common_irq, output_irq;
    uint32_t recovery_status, recovery_sequence;
} dma_frame_status_t;

void dma_read_frame_status(dma_frame_status_t *status);
/*
 * Lớp frame phải sở hữu ISP và điều phối vòng slot. Chỉ cấp một frame khi DMA đã tắt/rỗi,
 * không còn VALID/FREE/DONE. Ghi metadata, cấp output trước input, rồi mới bật hai DMA.
 * Lỗi sau khi cấp quyền không tự thu hồi: caller giữ buffer tới khi drain/recovery được xác nhận.
 */
isp_status_t dma_submit_slot(uint8_t slot, uint8_t outputs, uint32_t tag, uint32_t timestamp);
/* Chỉ tắt hai DMA khi pipeline, VALID và DMA đều đã rỗi; giữ nguyên vị trí vòng slot. */
isp_status_t dma_pause(void);
/* Xác nhận DONE bằng W1C khi DMA đã tắt/rỗi; chưa cấp FREE lại. Có thể thử lại khi W1C bị giữ. */
isp_status_t dma_release_slot(uint8_t slot, uint8_t outputs);
#endif
