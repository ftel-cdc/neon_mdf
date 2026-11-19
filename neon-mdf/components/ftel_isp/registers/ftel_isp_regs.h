/*
 * Tên viết tắt, trường bit, giới hạn và macro MMIO dùng bởi driver.
 * Mọi offset R_* và phiên bản ABI lấy từ các header regmap-v1.07-*.h.
 */
#ifndef FTEL_ISP_REGS_H
#define FTEL_ISP_REGS_H

#include <stdint.h>
#include <stdbool.h>
#include "regmap-v1.07-base.h"
#include "regmap-v1.07-common.h"
#include "regmap-v1.07-blc.h"
#include "regmap-v1.07-lsc.h"
#include "regmap-v1.07-bpc.h"
#include "regmap-v1.07-wb.h"
#include "regmap-v1.07-dg.h"
#include "regmap-v1.07-demosaic.h"
#include "regmap-v1.07-ccm.h"
#include "regmap-v1.07-gamma.h"
#include "regmap-v1.07-csc.h"
#include "regmap-v1.07-gtm.h"
#include "regmap-v1.07-nr.h"
#include "regmap-v1.07-ee.h"
#include "regmap-v1.07-cnf.h"
#include "regmap-v1.07-aec.h"
#include "regmap-v1.07-awb.h"
#include "regmap-v1.07-af.h"
#include "regmap-v1.07-dma.h"
#include "regmap-v1.07-output.h"
#include "regmap-v1.07-tune.h"
#include "regmap-v1.07-layout.h"

/*
 * BSP phải cấp địa chỉ nền CSR đã ánh xạ bằng -DISP_BASE=... hoặc header nền tảng.
 * Không có địa chỉ mặc định: regmap của IP chỉ quy định offset, không quy định địa chỉ SoC.
 */
#ifndef ISP_BASE
#error "BSP must define ISP_BASE as the mapped ISP CSR base address"
#endif

#if defined(__cplusplus)
static_assert(((uintptr_t)(ISP_BASE) & 3u) == 0u, "ISP_BASE must be 32-bit aligned");
#else
_Static_assert(((uintptr_t)(ISP_BASE) & 3u) == 0u, "ISP_BASE must be 32-bit aligned");
#endif

/*
 * Địa chỉ lệch thanh ghi theo byte; phần cứng bỏ qua hai bit địa chỉ thấp.
 */

/* ---- COMMON ---- */
#define R_COMMON_VER_DATE                FX1_ISP_COMMON_VER_DATE_OFFSET
#define R_COMMON_VER_ID                  FX1_ISP_COMMON_VER_ID_OFFSET
#define R_COMMON_CTRL                    FX1_ISP_COMMON_CTRL_OFFSET
#define R_COMMON_STATUS                  FX1_ISP_COMMON_STATUS_OFFSET
#define R_COMMON_IRQ_STATUS              FX1_ISP_COMMON_IRQ_STATUS_OFFSET
#define R_COMMON_IRQ_EN                  FX1_ISP_COMMON_IRQ_EN_OFFSET
#define R_COMMON_FRAME_WIDTH             FX1_ISP_COMMON_FRAME_WIDTH_OFFSET
#define R_COMMON_FRAME_HEIGHT            FX1_ISP_COMMON_FRAME_HEIGHT_OFFSET
#define R_COMMON_BAYER                   FX1_ISP_COMMON_BAYER_OFFSET
#define R_COMMON_SCRATCH                 FX1_ISP_COMMON_SCRATCH_OFFSET
#define R_COMMON_INPUT_FORMAT            FX1_ISP_COMMON_INPUT_FORMAT_OFFSET
#define R_COMMON_RECOVERY_STATUS         FX1_ISP_COMMON_RECOVERY_STATUS_OFFSET
#define R_COMMON_RECOVERY_SEQUENCE       FX1_ISP_COMMON_RECOVERY_SEQUENCE_OFFSET

/* Thanh ghi điều khiển/trạng thái DMA dùng cho khởi tạo và kiểm tra rỗi, theo CSR v1.07. */
#define R_DMA_CTRL                       FX1_ISP_DMA_CTRL_OFFSET
#define R_DMA_STAT                       FX1_ISP_DMA_STAT_OFFSET
#define R_DMA_ERR                        FX1_ISP_DMA_ERR_OFFSET
#define R_DMA_IRQ_EN                     FX1_ISP_DMA_IRQ_EN_OFFSET
#define R_IDMA_BUF_VALID                 FX1_ISP_IDMA_BUF_VALID_OFFSET
#define DMA_CTRL_ENABLE_MASK        0x3u
#define DMA_CTRL_BURST_MASK         0xFF00u
#define DMA_CTRL_RESET_VALUE        0x3F00u
#define DMA_STAT_BUSY_MASK          0x3u
#define IDMA_BUF_VALID_MASK         FX1_ISP_IDMA_BUF_VALID_IDMA_BUF_VALID_MASK

/* ---- BLC ---- */
#define R_BLC_CTRL                       FX1_ISP_BLC_CTRL_OFFSET
#define R_BLC_GAIN_SEL                   FX1_ISP_BLC_GAIN_SEL_OFFSET

#define R_BLC_OFS_G0_DFT                 FX1_ISP_BLC_OFS_G0_DFT_OFFSET
#define R_BLC_OFS_G0_R                   FX1_ISP_BLC_OFS_G0_R_OFFSET
#define R_BLC_OFS_G0_GR                  FX1_ISP_BLC_OFS_G0_GR_OFFSET
#define R_BLC_OFS_G0_GB                  FX1_ISP_BLC_OFS_G0_GB_OFFSET
#define R_BLC_OFS_G0_B                   FX1_ISP_BLC_OFS_G0_B_OFFSET

#define R_BLC_OFS_G1_DFT                 FX1_ISP_BLC_OFS_G1_DFT_OFFSET
#define R_BLC_OFS_G1_R                   FX1_ISP_BLC_OFS_G1_R_OFFSET
#define R_BLC_OFS_G1_GR                  FX1_ISP_BLC_OFS_G1_GR_OFFSET
#define R_BLC_OFS_G1_GB                  FX1_ISP_BLC_OFS_G1_GB_OFFSET
#define R_BLC_OFS_G1_B                   FX1_ISP_BLC_OFS_G1_B_OFFSET

#define R_BLC_OFS_G2_DFT                 FX1_ISP_BLC_OFS_G2_DFT_OFFSET
#define R_BLC_OFS_G2_R                   FX1_ISP_BLC_OFS_G2_R_OFFSET
#define R_BLC_OFS_G2_GR                  FX1_ISP_BLC_OFS_G2_GR_OFFSET
#define R_BLC_OFS_G2_GB                  FX1_ISP_BLC_OFS_G2_GB_OFFSET
#define R_BLC_OFS_G2_B                   FX1_ISP_BLC_OFS_G2_B_OFFSET

#define R_BLC_OFS_G3_DFT                 FX1_ISP_BLC_OFS_G3_DFT_OFFSET
#define R_BLC_OFS_G3_R                   FX1_ISP_BLC_OFS_G3_R_OFFSET
#define R_BLC_OFS_G3_GR                  FX1_ISP_BLC_OFS_G3_GR_OFFSET
#define R_BLC_OFS_G3_GB                  FX1_ISP_BLC_OFS_G3_GB_OFFSET
#define R_BLC_OFS_G3_B                   FX1_ISP_BLC_OFS_G3_B_OFFSET


#define R_BLC_OFS_G4_DFT FX1_ISP_BLC_OFS_G4_DFT_OFFSET
#define R_BLC_OFS_G4_R FX1_ISP_BLC_OFS_G4_R_OFFSET
#define R_BLC_OFS_G4_GR FX1_ISP_BLC_OFS_G4_GR_OFFSET
#define R_BLC_OFS_G4_GB FX1_ISP_BLC_OFS_G4_GB_OFFSET
#define R_BLC_OFS_G4_B FX1_ISP_BLC_OFS_G4_B_OFFSET
#define R_BLC_OFS_G5_DFT FX1_ISP_BLC_OFS_G5_DFT_OFFSET
#define R_BLC_OFS_G5_R FX1_ISP_BLC_OFS_G5_R_OFFSET
#define R_BLC_OFS_G5_GR FX1_ISP_BLC_OFS_G5_GR_OFFSET
#define R_BLC_OFS_G5_GB FX1_ISP_BLC_OFS_G5_GB_OFFSET
#define R_BLC_OFS_G5_B FX1_ISP_BLC_OFS_G5_B_OFFSET
#define R_BLC_OFS_G6_DFT FX1_ISP_BLC_OFS_G6_DFT_OFFSET
#define R_BLC_OFS_G6_R FX1_ISP_BLC_OFS_G6_R_OFFSET
#define R_BLC_OFS_G6_GR FX1_ISP_BLC_OFS_G6_GR_OFFSET
#define R_BLC_OFS_G6_GB FX1_ISP_BLC_OFS_G6_GB_OFFSET
#define R_BLC_OFS_G6_B FX1_ISP_BLC_OFS_G6_B_OFFSET
#define R_BLC_OFS_G7_DFT FX1_ISP_BLC_OFS_G7_DFT_OFFSET
#define R_BLC_OFS_G7_R FX1_ISP_BLC_OFS_G7_R_OFFSET
#define R_BLC_OFS_G7_GR FX1_ISP_BLC_OFS_G7_GR_OFFSET
#define R_BLC_OFS_G7_GB FX1_ISP_BLC_OFS_G7_GB_OFFSET
#define R_BLC_OFS_G7_B FX1_ISP_BLC_OFS_G7_B_OFFSET
#define R_BLC_SCALE_G0_R FX1_ISP_BLC_SCALE_G0_R_OFFSET
#define R_BLC_SCALE_G0_GR FX1_ISP_BLC_SCALE_G0_GR_OFFSET
#define R_BLC_SCALE_G0_GB FX1_ISP_BLC_SCALE_G0_GB_OFFSET
#define R_BLC_SCALE_G0_B FX1_ISP_BLC_SCALE_G0_B_OFFSET
#define R_BLC_SCALE_G1_R FX1_ISP_BLC_SCALE_G1_R_OFFSET
#define R_BLC_SCALE_G1_GR FX1_ISP_BLC_SCALE_G1_GR_OFFSET
#define R_BLC_SCALE_G1_GB FX1_ISP_BLC_SCALE_G1_GB_OFFSET
#define R_BLC_SCALE_G1_B FX1_ISP_BLC_SCALE_G1_B_OFFSET
#define R_BLC_SCALE_G2_R FX1_ISP_BLC_SCALE_G2_R_OFFSET
#define R_BLC_SCALE_G2_GR FX1_ISP_BLC_SCALE_G2_GR_OFFSET
#define R_BLC_SCALE_G2_GB FX1_ISP_BLC_SCALE_G2_GB_OFFSET
#define R_BLC_SCALE_G2_B FX1_ISP_BLC_SCALE_G2_B_OFFSET
#define R_BLC_SCALE_G3_R FX1_ISP_BLC_SCALE_G3_R_OFFSET
#define R_BLC_SCALE_G3_GR FX1_ISP_BLC_SCALE_G3_GR_OFFSET
#define R_BLC_SCALE_G3_GB FX1_ISP_BLC_SCALE_G3_GB_OFFSET
#define R_BLC_SCALE_G3_B FX1_ISP_BLC_SCALE_G3_B_OFFSET
#define R_BLC_SCALE_G4_R FX1_ISP_BLC_SCALE_G4_R_OFFSET
#define R_BLC_SCALE_G4_GR FX1_ISP_BLC_SCALE_G4_GR_OFFSET
#define R_BLC_SCALE_G4_GB FX1_ISP_BLC_SCALE_G4_GB_OFFSET
#define R_BLC_SCALE_G4_B FX1_ISP_BLC_SCALE_G4_B_OFFSET
#define R_BLC_SCALE_G5_R FX1_ISP_BLC_SCALE_G5_R_OFFSET
#define R_BLC_SCALE_G5_GR FX1_ISP_BLC_SCALE_G5_GR_OFFSET
#define R_BLC_SCALE_G5_GB FX1_ISP_BLC_SCALE_G5_GB_OFFSET
#define R_BLC_SCALE_G5_B FX1_ISP_BLC_SCALE_G5_B_OFFSET
#define R_BLC_SCALE_G6_R FX1_ISP_BLC_SCALE_G6_R_OFFSET
#define R_BLC_SCALE_G6_GR FX1_ISP_BLC_SCALE_G6_GR_OFFSET
#define R_BLC_SCALE_G6_GB FX1_ISP_BLC_SCALE_G6_GB_OFFSET
#define R_BLC_SCALE_G6_B FX1_ISP_BLC_SCALE_G6_B_OFFSET
#define R_BLC_SCALE_G7_R FX1_ISP_BLC_SCALE_G7_R_OFFSET
#define R_BLC_SCALE_G7_GR FX1_ISP_BLC_SCALE_G7_GR_OFFSET
#define R_BLC_SCALE_G7_GB FX1_ISP_BLC_SCALE_G7_GB_OFFSET
#define R_BLC_SCALE_G7_B FX1_ISP_BLC_SCALE_G7_B_OFFSET

/* ---- LSC ---- */
#define R_LSC_CTRL                       FX1_ISP_LSC_CTRL_OFFSET
#define R_LSC_MESH_NODES                 FX1_ISP_LSC_MESH_NODES_OFFSET
#define R_LSC_STRENGTH                   FX1_ISP_LSC_STRENGTH_OFFSET
#define R_LSC_PROFILE_SEL                FX1_ISP_LSC_PROFILE_SEL_OFFSET
#define R_LSC_LOAD_CTRL                  FX1_ISP_LSC_LOAD_CTRL_OFFSET
#define R_LSC_COEF_DATA                  FX1_ISP_LSC_COEF_DATA_OFFSET
#define R_LSC_LOAD_STATUS                FX1_ISP_LSC_LOAD_STATUS_OFFSET
#define R_LSC_PROFILE_STATUS             FX1_ISP_LSC_PROFILE_STATUS_OFFSET
#define R_LSC_ERROR                      FX1_ISP_LSC_ERROR_OFFSET
#define R_LSC_STAT_OVF_CNT_R             FX1_ISP_LSC_STAT_OVF_CNT_R_OFFSET
#define R_LSC_STAT_OVF_CNT_GR            FX1_ISP_LSC_STAT_OVF_CNT_GR_OFFSET
#define R_LSC_STAT_OVF_CNT_GB            FX1_ISP_LSC_STAT_OVF_CNT_GB_OFFSET
#define R_LSC_STAT_OVF_CNT_B             FX1_ISP_LSC_STAT_OVF_CNT_B_OFFSET

/* ---- BPC ---- */
#define R_BPC_CTRL                       FX1_ISP_BPC_CTRL_OFFSET
#define R_BPC_MODE                       FX1_ISP_BPC_MODE_OFFSET
#define R_BPC_TEMPORAL_VAR               FX1_ISP_BPC_TEMPORAL_VAR_OFFSET
#define R_BPC_THRESH                     FX1_ISP_BPC_THRESH_OFFSET
#define R_BPC_PIXEL_AGE                  FX1_ISP_BPC_PIXEL_AGE_OFFSET
#define R_BPC_NUM_CANDIDATES             FX1_ISP_BPC_NUM_CANDIDATES_OFFSET
#define R_BPC_NUM_DEFECTIVE              FX1_ISP_BPC_NUM_DEFECTIVE_OFFSET
#define R_BPC_STATUS                     FX1_ISP_BPC_STATUS_OFFSET

/* ---- WB ---- */
#define R_WB_CTRL                        FX1_ISP_WB_CTRL_OFFSET
#define R_WB_GAIN_R                      FX1_ISP_WB_GAIN_R_OFFSET
#define R_WB_GAIN_G                      FX1_ISP_WB_GAIN_G_OFFSET
#define R_WB_GAIN_B                      FX1_ISP_WB_GAIN_B_OFFSET

/* ---- DG ---- */
#define R_DG_CTRL                        FX1_ISP_DG_CTRL_OFFSET
#define R_DG_GAIN                        FX1_ISP_DG_GAIN_OFFSET

/* ---- AEC (statistics) ---- */
#define R_AEC_CTRL                       FX1_ISP_AEC_CTRL_OFFSET
#define R_AEC_CONTEXT_ID                 FX1_ISP_AEC_CONTEXT_ID_OFFSET
#define R_AEC_ZONE_CFG                   FX1_ISP_AEC_ZONE_CFG_OFFSET
#define R_AEC_ZONE_SIZE                  FX1_ISP_AEC_ZONE_SIZE_OFFSET
#define R_AEC_SAMPLE_CLIP                FX1_ISP_AEC_SAMPLE_CLIP_OFFSET
#define R_AEC_THRESH                     FX1_ISP_AEC_THRESH_OFFSET
#define R_AEC_STATUS                     FX1_ISP_AEC_STATUS_OFFSET
#define R_AEC_FRAME_ID                   FX1_ISP_AEC_FRAME_ID_OFFSET
#define R_AEC_RESULT_CONTEXT_ID          FX1_ISP_AEC_RESULT_CONTEXT_ID_OFFSET
#define R_AEC_CHANNEL_SEL                FX1_ISP_AEC_CHANNEL_SEL_OFFSET
#define R_AEC_GLOBAL_SUM_LO              FX1_ISP_AEC_GLOBAL_SUM_LO_OFFSET
#define R_AEC_GLOBAL_SUM_HI              FX1_ISP_AEC_GLOBAL_SUM_HI_OFFSET
#define R_AEC_GLOBAL_COUNT               FX1_ISP_AEC_GLOBAL_COUNT_OFFSET
#define R_AEC_ZONE_ADDR                  FX1_ISP_AEC_ZONE_ADDR_OFFSET
#define R_AEC_ZONE_SUM                   FX1_ISP_AEC_ZONE_SUM_OFFSET
#define R_AEC_ZONE_COUNT                 FX1_ISP_AEC_ZONE_COUNT_OFFSET
#define R_AEC_ZONE_GREEN_OE_UE           FX1_ISP_AEC_ZONE_GREEN_OE_UE_OFFSET
#define R_AEC_ZONE_GREEN_MIN_MAX         FX1_ISP_AEC_ZONE_GREEN_MIN_MAX_OFFSET
#define R_AEC_HIST_ADDR                  FX1_ISP_AEC_HIST_ADDR_OFFSET
#define R_AEC_HIST_DATA                  FX1_ISP_AEC_HIST_DATA_OFFSET

/* ---- AWB (statistics) ---- */
#define R_AWB_CTRL                       FX1_ISP_AWB_CTRL_OFFSET
#define R_AWB_GLOBAL_SUM_R_H             FX1_ISP_AWB_GLOBAL_SUM_R_H_OFFSET
#define R_AWB_GLOBAL_SUM_R_L             FX1_ISP_AWB_GLOBAL_SUM_R_L_OFFSET
#define R_AWB_GLOBAL_SUM_G_H             FX1_ISP_AWB_GLOBAL_SUM_G_H_OFFSET
#define R_AWB_GLOBAL_SUM_G_L             FX1_ISP_AWB_GLOBAL_SUM_G_L_OFFSET
#define R_AWB_GLOBAL_SUM_B_H             FX1_ISP_AWB_GLOBAL_SUM_B_H_OFFSET
#define R_AWB_GLOBAL_SUM_B_L             FX1_ISP_AWB_GLOBAL_SUM_B_L_OFFSET
#define R_AWB_GLOBAL_COUNT               FX1_ISP_AWB_GLOBAL_COUNT_OFFSET
#define R_AWB_ZONE_ADDR                  FX1_ISP_AWB_ZONE_ADDR_OFFSET
#define R_AWB_ZONE_SUM_R_H               FX1_ISP_AWB_ZONE_SUM_R_H_OFFSET
#define R_AWB_ZONE_SUM_R_L               FX1_ISP_AWB_ZONE_SUM_R_L_OFFSET
#define R_AWB_ZONE_SUM_G_H               FX1_ISP_AWB_ZONE_SUM_G_H_OFFSET
#define R_AWB_ZONE_SUM_G_L               FX1_ISP_AWB_ZONE_SUM_G_L_OFFSET
#define R_AWB_ZONE_SUM_B_H               FX1_ISP_AWB_ZONE_SUM_B_H_OFFSET
#define R_AWB_ZONE_SUM_B_L               FX1_ISP_AWB_ZONE_SUM_B_L_OFFSET
#define R_AWB_ZONE_COUNT                 FX1_ISP_AWB_ZONE_COUNT_OFFSET
#define R_AWB_STATUS                     FX1_ISP_AWB_STATUS_OFFSET
#define R_AWB_UNDEREXPOSED_LIMIT         FX1_ISP_AWB_UNDEREXPOSED_LIMIT_OFFSET
#define R_AWB_SATURATION_LIMIT           FX1_ISP_AWB_SATURATION_LIMIT_OFFSET
#define R_AWB_CONTEXT_ID                 FX1_ISP_AWB_CONTEXT_ID_OFFSET
#define R_AWB_RESULT_CONTEXT_ID          FX1_ISP_AWB_RESULT_CONTEXT_ID_OFFSET
#define R_AWB_FRAME_ID                   FX1_ISP_AWB_FRAME_ID_OFFSET
#define R_AWB_RESULT_CTRL                FX1_ISP_AWB_RESULT_CTRL_OFFSET
#define R_AWB_RESULT_STATUS              FX1_ISP_AWB_RESULT_STATUS_OFFSET
#define R_AWB_DROP_COUNT                 FX1_ISP_AWB_DROP_COUNT_OFFSET

/* ---- TUNE: định danh và chế độ của phần mở rộng ---- */
#define R_TUNE_ID                        FX1_ISP_TUNE_ID_OFFSET
#define R_TUNE_REV                       FX1_ISP_TUNE_REV_OFFSET
#define R_TUNE_CAPS                      FX1_ISP_TUNE_CAPS_OFFSET
#define R_TUNE_MODE                      FX1_ISP_TUNE_MODE_OFFSET
#define R_TUNE_ERROR                     FX1_ISP_TUNE_ERROR_OFFSET /* Các bit lỗi đã chốt; ghi 1 để xóa (W1C). */

/* ---- TUNE: hình học kết quả AE/AWB đang giữ quyền đọc ---- */
#define R_TUNE_AEC_SOURCE                FX1_ISP_TUNE_AEC_SOURCE_OFFSET /* Điểm đo AE: 0=BPC, 1=DG. */
#define R_TUNE_AEC_RESULT_META           FX1_ISP_TUNE_AEC_RESULT_META_OFFSET /* Thông tin kết quả của bank đang giữ quyền đọc, không đổi trong thời gian giữ. */

#define R_TUNE_AEC_CMD                   FX1_ISP_TUNE_AEC_CMD_OFFSET
#define R_TUNE_AEC_STATUS                FX1_ISP_TUNE_AEC_STATUS_OFFSET
#define R_TUNE_AEC_DROPS                 FX1_ISP_TUNE_AEC_DROPS_OFFSET
#define R_TUNE_AEC_FRAME                 FX1_ISP_TUNE_AEC_FRAME_OFFSET
#define R_TUNE_AEC_PROFILE               FX1_ISP_TUNE_AEC_PROFILE_OFFSET
#define R_TUNE_AEC_INDEX                 FX1_ISP_TUNE_AEC_INDEX_OFFSET
#define R_TUNE_AEC_DATA_LO               FX1_ISP_TUNE_AEC_DATA_LO_OFFSET
#define R_TUNE_AEC_DATA_HI               FX1_ISP_TUNE_AEC_DATA_HI_OFFSET

#define R_TUNE_AE_FRAME_SIZE             FX1_ISP_TUNE_AE_FRAME_SIZE_OFFSET
#define R_TUNE_AE_GRID                   FX1_ISP_TUNE_AE_GRID_OFFSET
#define R_TUNE_AE_ZONE_SIZE              FX1_ISP_TUNE_AE_ZONE_SIZE_OFFSET
#define R_TUNE_AWB_FRAME_SIZE            FX1_ISP_TUNE_AWB_FRAME_SIZE_OFFSET

/*
 * Lớp truy cập thanh ghi MMIO.
 */
#define REG32(a)         (*(volatile uint32_t *)((uintptr_t)(ISP_BASE) + (uintptr_t)(a)))
#define REG_RD(a)        REG32(a)
#define REG_WR(a, v)     (REG32(a) = (uint32_t)(v))
#define REG_SET(a, m)    (REG32(a) |=  (uint32_t)(m))
#define REG_CLR(a, m)    (REG32(a) &= ~(uint32_t)(m))
#define REG_UPD(a, m, v) (REG32(a) = (REG32(a) & ~(uint32_t)(m)) \
                                  | ((uint32_t)(v) & (uint32_t)(m)))

/*
 * Định nghĩa các trường bit.
 */

/* COMMON_CTRL */
#define COMMON_CTRL_ISP_EN          (1u << 0)
#define COMMON_CTRL_SOFT_RST        (1u << 1)   /* W1SC */
#define COMMON_CTRL_FRAME_START     (1u << 2)   /* W1S; lệnh tương thích không tác động xử lý. */

/* COMMON_STATUS */
#define COMMON_STATUS_BUSY          (1u << 0)
#define COMMON_STATUS_FRAME_DONE    (1u << 1)
#define COMMON_STATUS_ERROR         (1u << 2)

/* CSR v1.07: trạng thái recovery có BUSY ở bit 0 và DONE ở bit 1 (W1C). */
#ifndef COMMON_RECOVERY_STATUS_BUSY
#define COMMON_RECOVERY_STATUS_BUSY (1u << 0)
#endif
#define COMMON_RECOVERY_STATUS_DONE (1u << 1)
#ifndef ISP_CHECK_RECOVERY_IDLE
#define ISP_CHECK_RECOVERY_IDLE     1
#endif

/* COMMON_INPUT_FORMAT.DEPTH[1:0] */
#define INPUT_DEPTH_RAW12           0u
#define INPUT_DEPTH_RAW10           1u
#define INPUT_DEPTH_INVALID2        2u
#define INPUT_DEPTH_INVALID3        3u

/* BLC_CTRL */
#define BLC_CTRL_EN                 (1u << 0)

/* BLC_GAIN_SEL */
#define BLC_GAIN_SEL_GAIN_LVL(m)    (((uint32_t)(m) << FX1_ISP_BLC_GAIN_SEL_GAIN_LEVEL_SHIFT) & FX1_ISP_BLC_GAIN_SEL_GAIN_LEVEL_MASK)
#define BLC_GAIN_SEL_RANGE_SCALE    (1u << 8)

/* LSC_CTRL */
#define LSC_CTRL_EN                 (1u << 0)

/* LSC_MESH_NODES */
#define LSC_MESH_NX(n)              (((uint32_t)(n) & 0xFFu) << 0)
#define LSC_MESH_NY(n)              (((uint32_t)(n) & 0xFFu) << 8)

/* LSC_STRENGTH */
#define LSC_STRENGTH_MASK           0x1FFFFu

/* LSC_PROFILE_SEL */
#define LSC_PROFILE_SEL_PROFILE     (0x3u << 0)

/* LSC_LOAD_CTRL */
#define LSC_LOAD_CTRL_PROFILE(p)    (((uint32_t)(p) & 0x3u) << 0)
#define LSC_LOAD_CTRL_BEGIN         (1u << 8)    /* W1S */
#define LSC_LOAD_CTRL_VALIDATE      (1u << 9)    /* W1S */
#define LSC_LOAD_CTRL_ABORT         (1u << 10)   /* W1S */

/* LSC_COEF_DATA */
#define LSC_COEF_DATA_MASK          FX1_ISP_LSC_COEF_DATA_COEF_DATA_MASK

/* LSC_LOAD_STATUS */
#define LSC_LOAD_STATUS_ADDR        0xFFFFu
#define LSC_LOAD_STATUS_FULL        (1u << 16)
#define LSC_LOAD_STATUS_BUSY        (1u << 17)
#define LSC_LOAD_STATUS_PROFILE     (0x3u << 18)

/* LSC_PROFILE_STATUS */
#define LSC_PROFILE_STATUS_VALID_MASK (0x7u << 0)
#define LSC_PROFILE_STATUS_VALID(p)   (1u << (p))
#define LSC_PROFILE_STATUS_ACTIVE     (0x3u << 4)
#define LSC_PROFILE_STATUS_ACTIVE_OK  (1u << 6)
#define LSC_PROFILE_STATUS_GEOM_OK    (1u << 7)

/* LSC_ERROR */
#define LSC_ERROR_ACTIVE_LOAD_REJECT  (1u << 0)
#define LSC_ERROR_PROFILE_SEL_REJECT  (1u << 1)
#define LSC_ERROR_COEF_RANGE          (1u << 2)
#define LSC_ERROR_COEF_COUNT          (1u << 3)
#define LSC_ERROR_GEOMETRY_INVALID    (1u << 4)
#define LSC_ERROR_LOAD_PROTOCOL       (1u << 5)
#define LSC_ERROR_STICKY_MASK         (0x3Fu)

/* BPC_CTRL */
#define BPC_CTRL_EN                 (1u << 0)

/* BPC_MODE */
#define BPC_MODE_DYNAMIC_DET_EN     (1u << 0)   /* Dự phòng, chưa có tác dụng xử lý. */

/* BPC_THRESH */
#define BPC_THRESH_K(k)             (((uint32_t)(k) & 0xFFFFu) << 16)
#define BPC_THRESH_FLOOR(f)         (((uint32_t)(f) & 0xFFFFu) << 0)

/* WB_GAIN (UQ4.8) / DG_GAIN */
#define GAIN_MASK                   0xFFFu

/* Ngưỡng AWB là mã mẫu RAW12, không phải gain UQ4.8. */
#define AWB_LIMIT_MASK              0xFFFu

/* WB_CTRL */
#define WB_CTRL_EN                  (1u << 0)

/* DG_CTRL */
#define DG_CTRL_EN                  (1u << 0)

/* AEC_CTRL */
#define AEC_CTRL_EN                 (1u << 0)
#define AEC_CTRL_COMMIT             (1u << 1)   /* W1S */

/* AEC_ZONE_CFG */
#define AEC_ZONE_CFG_NX(n)          (((uint32_t)(n) & 0x3Fu) << 0)
#define AEC_ZONE_CFG_NY(n)          (((uint32_t)(n) & 0x1Fu) << 8)

/* AEC_ZONE_SIZE */
#define AEC_ZONE_SIZE_W(w)          (((uint32_t)(w) & 0xFFFu) << 0)
#define AEC_ZONE_SIZE_H(h)          (((uint32_t)(h) & 0xFFFu) << 16)

/* AEC_SAMPLE_CLIP */
#define AEC_SAMPLE_CLIP_MIN(v)      (((uint32_t)(v) & 0xFFFu) << 0)
#define AEC_SAMPLE_CLIP_MAX(v)      (((uint32_t)(v) & 0xFFFu) << 16)

/* AEC_THRESH */
#define AEC_THRESH_UE(v)            (((uint32_t)(v) & 0xFFFu) << 0)
#define AEC_THRESH_OE(v)            (((uint32_t)(v) & 0xFFFu) << 16)

/* AEC_STATUS */
#define AEC_STATUS_BUSY             (1u << 0)
#define AEC_STATUS_STAT_DONE        (1u << 1)   /* W1C */
#define AEC_STATUS_ERROR            (1u << 2)   /* W1C */

/* AEC_ZONE_GREEN_OE_UE / MIN_MAX */
#define AEC_ZONE_UE_COUNT(v)        (((uint32_t)(v) & 0xFFFFu) << 0)
#define AEC_ZONE_OE_COUNT(v)        (((uint32_t)(v) & 0xFFFFu) << 16)
#define AEC_ZONE_MIN(v)             (((uint32_t)(v) & 0xFFFu) << 0)
#define AEC_ZONE_MAX(v)             (((uint32_t)(v) & 0xFFFu) << 12)

/* AWB_CTRL */
#define AWB_CTRL_EN                 (1u << 0)
#define AWB_CTRL_NX(n)              (((uint32_t)(n) & 0x7Fu) << 1)
#define AWB_CTRL_NY(n)              (((uint32_t)(n) & 0x3Fu) << 8)

/* AWB_ZONE_ADDR: địa chỉ zone 11 bit. */
#define AWB_ZONE_ADDR_MAX           0x7FFu
#define AWB_ZONE_ADDR_MASK          0x7FFu

/* AWB_STATUS */
#define AWB_STATUS_STAT_DONE        (1u << 0)   /* W1C */

/* AWB_RESULT_CTRL: các lệnh xung W1P. */
#define AWB_RESULT_CTRL_ACQUIRE     (1u << 0)
#define AWB_RESULT_CTRL_RELEASE     (1u << 1)
#define AWB_RESULT_CTRL_CLEAR_REPORT (1u << 2)

/* AWB_RESULT_STATUS */
#define AWB_RESULT_STATUS_READY_COUNT (0x3u << 0)
#define AWB_RESULT_STATUS_LEASED      (1u << 2)
#define AWB_RESULT_STATUS_READ_VALID  (1u << 3)
#define AWB_RESULT_STATUS_READ_BANK   (1u << 4)
#define AWB_RESULT_STATUS_WRITE_BUSY  (1u << 5)
#define AWB_RESULT_STATUS_OVERRUN     (1u << 6)
#define AWB_RESULT_STATUS_RD_NX       (0x7Fu << 8)
#define AWB_RESULT_STATUS_RD_NY       (0x3Fu << 16)

/* Hằng nhận diện TUNE_ID. */
#define TUNE_ID_MAGIC               0x54554E45u

/* TUNE_CAPS */
#define TUNE_CAP_PROFILE            (1u << 0)
#define TUNE_CAP_NR                 (1u << 1)
#define TUNE_CAP_AWB_COLOR          (1u << 2)
#define TUNE_CAP_AF_WINDOWS         (1u << 3)
#define TUNE_CAP_HIST_WEIGHT        (1u << 4)
#define TUNE_CAP_WB_GB              (1u << 5)
#define TUNE_CAP_AEC_LEASE          (1u << 9)
#define TUNE_CAP_AF_LEASE           (1u << 13)
#define TUNE_CAP_FULL_V107          0x003FFFFFu

/* TUNE_MODE */
#define TUNE_MODE_ENABLE            (1u << 0)

/* Vị trí bit lỗi TUNE_ERROR theo bảng Flat Registers của CSR v1.07. */
#define TUNE_ERROR_LOCKED           (1u << 0)
#define TUNE_ERROR_MODE             (1u << 5)
#define TUNE_ERROR_STICKY_MASK      0xFFFFFFFFu

/* TUNE_AEC_SOURCE */
#define TUNE_AEC_SOURCE_BPC         0u
#define TUNE_AEC_SOURCE_DG          1u
#define TUNE_AEC_SOURCE_MASK        0x1u

/* TUNE_AEC_RESULT_META */
#define TUNE_AEC_META_SOURCE        (1u << 0)
#define TUNE_AEC_META_DOMAIN        (0xFu << 4)

/* TUNE_AEC_CMD */
#define TUNE_AEC_CMD_ACQUIRE        (1u << 0)
#define TUNE_AEC_CMD_RELEASE        (1u << 1)

/* TUNE_AEC_STATUS */
#define TUNE_AEC_STATUS_LEASE_VALID (1u << 0)
#define TUNE_AEC_STATUS_READY_COUNT (0x3u << 1)
#define TUNE_AEC_STATUS_BUSY        (1u << 3)

/* TUNE_AE_FRAME_SIZE / TUNE_AWB_FRAME_SIZE */
#define AE_FRAME_SIZE_W(v)          (((uint32_t)(v) & 0x1FFFu) << 0)
#define AE_FRAME_SIZE_H(v)          (((uint32_t)(v) & 0x1FFFu) << 16)

/* TUNE_AE_GRID */
#define AE_GRID_NX(v)               (((uint32_t)(v) & 0x3Fu) << 0)
#define AE_GRID_NY(v)               (((uint32_t)(v) & 0x1Fu) << 16)

/* TUNE_AE_ZONE_SIZE */
#define AE_ZONE_SIZE_W(v)           (((uint32_t)(v) & 0xFFFFu) << 0)
#define AE_ZONE_SIZE_H(v)           (((uint32_t)(v) & 0xFFFFu) << 16)

/*
 * Các kiểu liệt kê công khai.
 */
typedef enum {
    BAYER_RGGB = 0,
    BAYER_GRBG = 1,
    BAYER_GBRG = 2,
    BAYER_BGGR = 3,
} bayer_pattern_t;

/*
 * Giới hạn phần cứng.
 */
#define IFMT_W_MIN                  32u
#define IFMT_W_MAX                  3842u
#define IFMT_H_MIN                  32u
#define IFMT_H_MAX                  2162u
#define IFMT_ACTIVE_W_MAX           3840u
#define IFMT_ACTIVE_H_MAX           2160u

#define BLC_PEDESTAL_MAX            4095u
#define BLC_TRIM_MAX                4095u
#define BLC_SCALE_UNITY             0x10000u
#define BLC_SCALE_MAX               0x3FFFFu

/* LSC */
#define LSC_NUM_PROFILES            3u
#define LSC_PROFILE_MAX             2u
#define LSC_MESH_N_MIN              2u
#define LSC_MESH_N_MAX              32u
#define LSC_COEFS_PER_NODE          4u
#define LSC_STRENGTH_UNITY          0x10000u
#define LSC_STRENGTH_DEFAULT        0x10000u
#define LSC_COEF_UNITY              0x040000u   /* UQ4.18; hệ số hợp lệ 0..8. */
#define LSC_COEF_MAX                0x200000u   /* ISP-UPD-001: hệ số tối đa 8x. */
#define LSC_LOAD_POLL_MAX           100000u

/* BPC */
#define BPC_THRESH_K_DEFAULT        0x004Du
#define BPC_THRESH_FLOOR_DEFAULT    0x0192u
#define BPC_COUNT_MAX               16383u

/* WB / DG (UQ4.8) */
#define GAIN_UNITY                  0x100u
#define GAIN_MAX                    0xFFFu

/* AEC */
#define AEC_NUM_ZONE_X_MAX          32u
#define AEC_NUM_ZONE_Y_MAX          24u
#define AEC_NUM_ZONE_MAX            768u
#define AEC_ZONE_DIM_MAX            4095u
#define AEC_SAMPLE_MAX              4095u
#define AEC_THRESH_UE_DEFAULT       272u
#define AEC_THRESH_OE_DEFAULT       3744u
#define AEC_HIST_BINS               64u

/* Các chỉ số đọc kết quả AEC 64 bit của bank đang giữ quyền, theo MAS Bảng 194. */
#define AEC_IDX_GLOBAL_SUM          0u
#define AEC_IDX_GLOBAL_COUNT        1u
#define AEC_IDX_GLOBAL_OE           2u
#define AEC_IDX_GLOBAL_UE           3u
#define AEC_IDX_ZONE_BASE           0x200u   /* Mỗi zone chiếm thêm 8*z: tổng, số mẫu, OE, UE, min, max, 0, 0. */
#define AEC_IDX_OVERFLOW            8u       /* Bit 0 báo có bộ tích lũy bị tràn. */

/* Thống kê AWB. */
#define AWB_NUM_ZONE_X_MAX          64u      /* Giới hạn ngang MAX_ZONE_X theo Bảng 133 của MAS. */
#define AWB_NUM_ZONE_Y_MAX          32u      /* Giới hạn dọc MAX_ZONE_Y theo Bảng 133 của MAS. */
#define AWB_NUM_ZONE_MAX            2048u    /* MAS 6.22.3.7: tối đa 64 x 32 zone. */
#define AWB_ZONE_INDEX_MAX          2047u    /* Địa chỉ zone rộng 11 bit. */

/* Giới hạn thăm dò cho các ghi lớp IDLE, theo MAS 9.9.3. */
#define ISP_IDLE_DRAIN_TIMEOUT      100000u

/* ABI */
#define ISP_ABI_VERSION             FX1_ISP_EXPECTED_VER_ID

#endif /* FTEL_ISP_REGS_H */
