/**
 * @file    neon_csi.c
 * @brief   NEON CSI-2 receiver driver implementation for FX1.
 * @details Register map: peripheral MIPI_CSI2 of FX1.yaml (fx1_registers.h), MAS v1.4. Application-side
 * notes are in neon_csi_fx1.h.
 *
 * Hardware traps to respect in this file:
 * - init() does not touch the hardware, and a warm restart may leave the DMA writing and the IRQ enabled.
 *   configure() therefore starts by quieting the receiver: GLOBAL_IRQ_ENABLE and IRQ_ENABLE to 0, the DMA
 *   aborted and stopped, CORE_CONFIG.core_enable to 0 (its reset value is 1), IRQ_STATUS cleared.
 * - MAS 9.1 order: configure() does steps 7 to 9 with dma_enable 0; start() does the rest of 9 (dma_enable,
 *   dma_start), 10 (core_enable) and 11 (clear IRQ_STATUS, then IRQ_ENABLE, then global_enable). Steps 2 to
 *   6 and 13 belong to neon_mipi (D-PHY): configure() only holds a placeholder that confirms the link is
 *   ready, csi_dphy_bringup(). Of step 4, configure() writes vc_exposure_map (read-modify-write, the same
 *   register holds active_lanes). Step 12 belongs to the application. With the test pattern generator, start()
 *   also writes vc_exposure_map = 0, TPG_RESOLUTION, TPG_BLANKING, then tpg_enable, and waits for
 *   CORE_STATUS.active_src = 1 and src_switch_pending = 0.
 * - soft_reset needs the byte clock (PHY_STATUS.cl_rxclkactivehs); without it, escalate to the link.
 *   full_reset only while DMA_STATUS.dma_busy is 0, and it clears every register: configure again.
 * - Events during a reset and the 256 sys_clk after it are not recorded. Every soft_reset leaves one
 *   spurious LANE_ALIGN_ABORT: write 0x00FFFFFF to IRQ_STATUS again afterwards.
 * - A write to IRQ_STATUS completes only once the bits read back as 0 (up to 16 lite_aclk).
 * - Read TIMESTAMP_LO before TIMESTAMP_HI: the LO read latches HI into a shadow.
 * - A read of GENERIC_SHORT_PACKET pops the FIFO; an empty FIFO reads 0.
 * - BUFFER_OWNERSHIP may still show the old state right after BUFFER_RELEASE (clock domain crossing).
 * - BUFFER_ERROR_INFO 0xFFFF means no valid line: report UINT32_MAX in frame.last_valid_line.
 * - DMA_CONTROL.dma_format changes only while dma_enable and dma_busy are 0. To stop, clear dma_start
 *   before dma_enable.
 * - LINE_PITCH holds bytes_per_line [15:0] and line_padding [23:16]; config.dma.line_pitch_bytes is their sum.
 * - EMB_EXTRACT_CFGm.len is 1 to 4; 0 is rejected.
 * - Not in MAS/HAS, take from the CSR workbook before use: PROTOCOL_CONFIG.max_lanes, CORE_STATUS.core_state,
 *   LANE_STATUS, EMBEDDED_DATA_INFO_VCn bit positions.
 *
 * IRQ_STATUS bit -> event / sub event (FX1_ = NEON_CSI_FX1_SUB_EVENT_, others NEON_CSI_SUB_EVENT_):
 *   0, 2  FRAME_LEVEL_ERR_VC0/1  -> FRAME   / FRAME_ERROR_VC(0/1)
 *   1, 3  FRAME_SYNC_ERR_VC0/1   -> FRAME   / FRAME_SYNC_ERROR_VC(0/1)
 *   4     CRC_ERROR              -> PACKET  / PACKET_CRC_ERROR
 *   5     ECC_1BIT_ERROR         -> PACKET  / PACKET_ECC_CORRECTED
 *   6     ECC_2BIT_ERROR         -> PACKET  / PACKET_ECC_UNCORRECTABLE
 *   7     UNSUPPORTED_DT         -> PACKET  / PACKET_UNSUPPORTED_DT
 *   8     WC_CORRUPTION          -> PACKET  / PACKET_WC_CORRUPTION
 *   9     SOT_ERROR              -> PHY     / PHY_SOT_ERROR
 *   10    SOT_SYNC_ERROR         -> PHY     / PHY_SOT_SYNC_ERROR
 *   11    STOP_STATE             -> PLATFORM / FX1_CLOCK_STOP_STATE
 *   12    LINE_BUFFER_FULL       -> PLATFORM / FX1_LINE_BUFFER_FULL
 *   13    BUFFER_OVERFLOW        -> SYSTEM  / SYSTEM_OVERFLOW
 *   14    FRAME_DROPPED          -> BUFFER  / BUFFER_DROPPED
 *   15    FRAME_RECEIVED         -> PLATFORM / FX1_STREAM_FRAME_RECEIVED
 *   16    BUF_DONE_OK            -> BUFFER  / BUFFER_VALID
 *   17    BUF_DONE_ERR           -> BUFFER  / BUFFER_ERROR
 *   18    AXI_WRITE_ERROR        -> SYSTEM  / SYSTEM_BUS_ERROR
 *   19    LANE_ALIGN_ABORT       -> PLATFORM / FX1_LANE_ABORT
 *   20    RESOLUTION_MISMATCH    -> PLATFORM / FX1_RESOLUTION_MISMATCH
 *   21    ERR_WC_ALIGN           -> PLATFORM / FX1_WC_ALIGN_ERROR
 *   22    FMTBRIDGE_OVERFLOW     -> PLATFORM / FX1_FORMAT_BRIDGE_OVERFLOW
 *   23    FRAME_NUM_SEQ_ERR      -> PLATFORM / FX1_FRAME_NUMBER_ERROR
 *   SYSTEM_LINK_RESET is raised by this driver; FRAME_START and PACKET_SHORT have no source.
 */

/*==================================================================================================
*                                        INCLUDE FILES
==================================================================================================*/
#include "csi/platform/fx1/neon_csi_fx1.h"
#include "fx1_fw.h"
#include "fx1_registers.h"

/*==================================================================================================
*                                        LOCAL MACROS
==================================================================================================*/
#define CSI_REG(name) ((uintptr_t)NEON_FX1_MIPI_CSI2_BASE + NEON_FX1_MIPI_CSI2_##name##_OFFSET)

/* Limits of the FX1 receiver (MAS v1.4). */
#define CSI_DMA_ALIGN           8U      /* Addresses, sizes and the stride: bits 2:0 are ignored. */
#define CSI_MAX_BYTES_PER_LINE  0xFFFFU /* LINE_PITCH.bytes_per_line is 16 bits. */
#define CSI_MAX_LINE_PADDING    0xFFU   /* LINE_PITCH.line_padding is 8 bits. */
#define CSI_META_PAYLOAD_OFFSET 0x80U   /* Embedded payload offset in a metadata area; also its minimum size. */
#define CSI_TPG_WIDTH_STEP      4U      /* The generator rounds its width down to a multiple of this. */
#define CSI_TPG_MIN_SIZE        8U      /* Smallest generator width and height. */
#define CSI_DEFAULT_METADATA_DT 0x12U   /* Embedded 8-bit non-image data; METADATA_DT_CONFIG reset value. */

/* Polling budgets. One poll is one CSR read. */
#define CSI_DPHY_READY_POLLS    100000U /* Stop State wait (MAS 9.1 step 6); T_INIT is at least 100 us. */
#define CSI_DMA_IDLE_POLLS      100000U /* DMA_STATUS.dma_busy after dma_abort: in-flight AXI bursts must drain. */
#define CSI_RESET_POLLS         100000U /* CORE_STATUS.reset_in_progress after soft_reset. */
#define CSI_TPG_SWITCH_POLLS    100000U /* CORE_STATUS source switch to the TPG; MAS gives no fixed maximum. */

/* Field value: shift then mask, so an out-of-range value cannot touch the next field. */
#define CSI_FIELD(reg, field, value)                                                                  \
    (((uint32_t)(value) << NEON_FX1_MIPI_CSI2_##reg##_##field##_SHIFT) & NEON_FX1_MIPI_CSI2_##reg##_##field##_MASK)
#define CSI_BIT(reg, field) NEON_FX1_MIPI_CSI2_##reg##_##field##_MASK
/* Field of a register value read back, moved down to bit 0. */
#define CSI_GET(reg, field, value)                                                                    \
    (((uint32_t)(value) & NEON_FX1_MIPI_CSI2_##reg##_##field##_MASK) >> NEON_FX1_MIPI_CSI2_##reg##_##field##_SHIFT)

/* Metadata record in DDR, at the start of each channel's metadata area (MAS Table 6-49). */
#define CSI_RECORD_BYTES        0x28U   /* Receiver fields, words 0 to 9; the DMA writes nothing else below 0x80. */
#define CSI_RECORD_WORD_INFO    0U      /* version [7:0], vc [9:8], err_summary [20:16]. */
#define CSI_RECORD_WORD_FRAME   1U      /* frame_id [13:0]. */
#define CSI_RECORD_WORD_TS_LO   2U      /* timestamp [31:0]. */
#define CSI_RECORD_WORD_TS_HI   3U      /* timestamp [63:32]. */
#define CSI_RECORD_WORD_GEOM    5U      /* byte_count [15:0], line_count [31:16]. */
#define CSI_RECORD_WORD_EMB     6U      /* emb_word_count [15:0], emb_line_count [23:16], data_type [29:24]. */
#define CSI_RECORD_WORD_EXTRACT 7U      /* Extraction slot 0; slot 1 follows. */

/* frame_id: 14 bits, from 1, skipping 0 when it wraps (MAS 9.3 buffer service step 2). */
#define CSI_FRAME_ID_PERIOD     0x3FFFU
#define CSI_SHORT_PACKET_EMPTY  0U      /* GENERIC_SHORT_PACKET.data_type when the FIFO was empty. */
#define CSI_EXTRACT_MAX_LEN     4U      /* EMB_EXTRACT_CFGm.len is 1 to 4. */

/* IP version this driver is written for (MAS 9.1 step 1). */
#define CSI_EXPECTED_IP_VERSION NEON_FX1_MIPI_CSI2_IP_VERSION_RESET

/* Sub events FX1 can raise, per event. FRAME_START and PACKET_SHORT have no interrupt source on FX1. */
#define CSI_SUPPORTED_BUFFER                                                                          \
    (NEON_CSI_SUB_EVENT_BUFFER_VALID | NEON_CSI_SUB_EVENT_BUFFER_ERROR | NEON_CSI_SUB_EVENT_BUFFER_DROPPED)
#define CSI_SUPPORTED_FRAME                                                                           \
    (NEON_CSI_SUB_EVENT_FRAME_ERROR_VC(0) | NEON_CSI_SUB_EVENT_FRAME_ERROR_VC(1) |                    \
     NEON_CSI_SUB_EVENT_FRAME_SYNC_ERROR_VC(0) | NEON_CSI_SUB_EVENT_FRAME_SYNC_ERROR_VC(1))
#define CSI_SUPPORTED_PACKET                                                                          \
    (NEON_CSI_SUB_EVENT_PACKET_ECC_CORRECTED | NEON_CSI_SUB_EVENT_PACKET_ECC_UNCORRECTABLE |          \
     NEON_CSI_SUB_EVENT_PACKET_CRC_ERROR | NEON_CSI_SUB_EVENT_PACKET_UNSUPPORTED_DT |                 \
     NEON_CSI_SUB_EVENT_PACKET_WC_CORRUPTION)
#define CSI_SUPPORTED_PHY (NEON_CSI_SUB_EVENT_PHY_SOT_ERROR | NEON_CSI_SUB_EVENT_PHY_SOT_SYNC_ERROR)
#define CSI_SUPPORTED_SYSTEM                                                                          \
    (NEON_CSI_SUB_EVENT_SYSTEM_LINK_RESET | NEON_CSI_SUB_EVENT_SYSTEM_OVERFLOW |                      \
     NEON_CSI_SUB_EVENT_SYSTEM_BUS_ERROR)
#define CSI_SUPPORTED_PLATFORM                                                                        \
    (NEON_CSI_FX1_SUB_EVENT_CLOCK_STOP_STATE | NEON_CSI_FX1_SUB_EVENT_LINE_BUFFER_FULL |              \
     NEON_CSI_FX1_SUB_EVENT_LANE_ABORT | NEON_CSI_FX1_SUB_EVENT_WC_ALIGN_ERROR |                      \
     NEON_CSI_FX1_SUB_EVENT_FORMAT_BRIDGE_OVERFLOW | NEON_CSI_FX1_SUB_EVENT_STREAM_FRAME_RECEIVED |   \
     NEON_CSI_FX1_SUB_EVENT_RESOLUTION_MISMATCH | NEON_CSI_FX1_SUB_EVENT_FRAME_NUMBER_ERROR)

/*==================================================================================================
*                                    LOCAL TYPEDEFS (STRUCTURES, UNIONS, ENUMS)
==================================================================================================*/
/** One IRQ_STATUS source and the sub event it raises. */
typedef struct {
    uint32_t irq_bit;                          /* IRQ_STATUS / IRQ_ENABLE bit, a _MASK of fx1_registers.h. */
    neon_csi_event_t event;                    /* Event the source belongs to. */
    uint32_t sub_event;                        /* Sub event bit within that event. */
} csi_irq_map_t;

/*==================================================================================================
*                                      LOCAL VARIABLES
==================================================================================================*/
/* What FX1 provides (MAS v1.4) */
static const neon_csi_caps_t fx1_caps = {
    .buffer_count = (uint8_t)NEON_CSI_FX1_BUFFER_COUNT, /* Ping-pong slots of the Buffer Manager. */
    .vc_count = (uint8_t)NEON_CSI_FX1_VC_COUNT,         /* VC0 and VC1; other channels are ignored. */
    .has_test_pattern = true,                           /* TPG_CONTROL. */
    .has_metadata = true,                               /* METADATA_DT_CONFIG.metadata_capture_en. */
    /* The Format Bridge takes RAW10 and RAW12 only (STREAM_CONFIG.stream_format, DMA_CONTROL.dma_format). */
    .supported_raw_formats = NEON_CSI_RAW_FORMAT_BIT(NEON_CSI_RAW10) | NEON_CSI_RAW_FORMAT_BIT(NEON_CSI_RAW12),
    /* FRAME_START and PACKET_SHORT have no interrupt source on FX1. */
    .supported_sub_events = {
        [NEON_CSI_EVENT_BUFFER] = CSI_SUPPORTED_BUFFER,
        [NEON_CSI_EVENT_FRAME] = CSI_SUPPORTED_FRAME,
        [NEON_CSI_EVENT_PACKET] = CSI_SUPPORTED_PACKET,
        [NEON_CSI_EVENT_PHY] = CSI_SUPPORTED_PHY,
        [NEON_CSI_EVENT_SYSTEM] = CSI_SUPPORTED_SYSTEM,
        [NEON_CSI_EVENT_PLATFORM] = CSI_SUPPORTED_PLATFORM,
    },
};

/* Starting point for neon_csi_configure() (MAS v1.4). Edit here; neon_csi_get_default_config() only
 * copies it. Fields not listed are 0: width/height, DMA addresses and sizes must come from the
 * application, metadata and the test pattern start disabled. */
static const neon_csi_config_t fx1_default_config = {
    .raw_format = NEON_CSI_RAW12,                   /* Most common FX1 sensor format. */
    .pixel_packing = NEON_CSI_PACKING_UNPACKED16,   /* One sample per 16-bit word, right aligned. */
    .metadata = {
        .data_type = CSI_DEFAULT_METADATA_DT,       /* 0x12, CSI-2 embedded data. Still disabled. */
    },
    /* Quiet by default: no FRAME_START, no corrected ECC, no PHY noise, no FX1 platform events. */
    .sub_event_mask = {
        [NEON_CSI_EVENT_BUFFER] = CSI_SUPPORTED_BUFFER,
        [NEON_CSI_EVENT_FRAME] = CSI_SUPPORTED_FRAME,
        [NEON_CSI_EVENT_PACKET] = NEON_CSI_SUB_EVENT_PACKET_ECC_UNCORRECTABLE | NEON_CSI_SUB_EVENT_PACKET_CRC_ERROR,
        [NEON_CSI_EVENT_SYSTEM] = CSI_SUPPORTED_SYSTEM,
    },
};

/* FX1 test pattern settings neon_csi_configure() programs. Edit here; neon_csi_fx1_set_tpg_config()
 * replaces them between configure() and start(). base is not used: configure() takes config.test_pattern. */
static const neon_csi_fx1_tpg_config_t fx1_default_tpg = {
    .base = {0},                               /* Replaced by config.test_pattern. */
    .cfa_order = NEON_CSI_FX1_CFA_RGGB,        /* TPG_CONTROL.tpg_cfa_order reset value. */
    .dual_vc = false,                          /* VC0 only: one channel per capture. */
    .hblank_cycles = 0U,                       /* Lines back to back; Line Buffer backpressure paces them. */
    .vblank_lines = 0U,                        /* Frames back to back. */
};

/* IRQ_STATUS -> event / sub event, the table at the top of this file. Used by start() for IRQ_ENABLE and
 * by the interrupt handler in the other direction. Every one of the 24 sources appears once. */
#define CSI_IRQ(name) NEON_FX1_MIPI_CSI2_IRQ_STATUS_##name##_MASK
static const csi_irq_map_t csi_irq_map[] = {
    {CSI_IRQ(FRAME_LEVEL_ERR_VC0), NEON_CSI_EVENT_FRAME, NEON_CSI_SUB_EVENT_FRAME_ERROR_VC(0)},
    {CSI_IRQ(FRAME_SYNC_ERR_VC0), NEON_CSI_EVENT_FRAME, NEON_CSI_SUB_EVENT_FRAME_SYNC_ERROR_VC(0)},
    {CSI_IRQ(FRAME_LEVEL_ERR_VC1), NEON_CSI_EVENT_FRAME, NEON_CSI_SUB_EVENT_FRAME_ERROR_VC(1)},
    {CSI_IRQ(FRAME_SYNC_ERR_VC1), NEON_CSI_EVENT_FRAME, NEON_CSI_SUB_EVENT_FRAME_SYNC_ERROR_VC(1)},
    {CSI_IRQ(CRC_ERROR), NEON_CSI_EVENT_PACKET, NEON_CSI_SUB_EVENT_PACKET_CRC_ERROR},
    {CSI_IRQ(ECC_1BIT_ERROR), NEON_CSI_EVENT_PACKET, NEON_CSI_SUB_EVENT_PACKET_ECC_CORRECTED},
    {CSI_IRQ(ECC_2BIT_ERROR), NEON_CSI_EVENT_PACKET, NEON_CSI_SUB_EVENT_PACKET_ECC_UNCORRECTABLE},
    {CSI_IRQ(UNSUPPORTED_DT), NEON_CSI_EVENT_PACKET, NEON_CSI_SUB_EVENT_PACKET_UNSUPPORTED_DT},
    {CSI_IRQ(WC_CORRUPTION), NEON_CSI_EVENT_PACKET, NEON_CSI_SUB_EVENT_PACKET_WC_CORRUPTION},
    {CSI_IRQ(SOT_ERROR), NEON_CSI_EVENT_PHY, NEON_CSI_SUB_EVENT_PHY_SOT_ERROR},
    {CSI_IRQ(SOT_SYNC_ERROR), NEON_CSI_EVENT_PHY, NEON_CSI_SUB_EVENT_PHY_SOT_SYNC_ERROR},
    {CSI_IRQ(STOP_STATE), NEON_CSI_EVENT_PLATFORM, NEON_CSI_FX1_SUB_EVENT_CLOCK_STOP_STATE},
    {CSI_IRQ(LINE_BUFFER_FULL), NEON_CSI_EVENT_PLATFORM, NEON_CSI_FX1_SUB_EVENT_LINE_BUFFER_FULL},
    {CSI_IRQ(BUFFER_OVERFLOW), NEON_CSI_EVENT_SYSTEM, NEON_CSI_SUB_EVENT_SYSTEM_OVERFLOW},
    {CSI_IRQ(FRAME_DROPPED), NEON_CSI_EVENT_BUFFER, NEON_CSI_SUB_EVENT_BUFFER_DROPPED},
    {CSI_IRQ(FRAME_RECEIVED), NEON_CSI_EVENT_PLATFORM, NEON_CSI_FX1_SUB_EVENT_STREAM_FRAME_RECEIVED},
    {CSI_IRQ(BUF_DONE_OK), NEON_CSI_EVENT_BUFFER, NEON_CSI_SUB_EVENT_BUFFER_VALID},
    {CSI_IRQ(BUF_DONE_ERR), NEON_CSI_EVENT_BUFFER, NEON_CSI_SUB_EVENT_BUFFER_ERROR},
    {CSI_IRQ(AXI_WRITE_ERROR), NEON_CSI_EVENT_SYSTEM, NEON_CSI_SUB_EVENT_SYSTEM_BUS_ERROR},
    {CSI_IRQ(LANE_ALIGN_ABORT), NEON_CSI_EVENT_PLATFORM, NEON_CSI_FX1_SUB_EVENT_LANE_ABORT},
    {CSI_IRQ(RESOLUTION_MISMATCH), NEON_CSI_EVENT_PLATFORM, NEON_CSI_FX1_SUB_EVENT_RESOLUTION_MISMATCH},
    {CSI_IRQ(ERR_WC_ALIGN), NEON_CSI_EVENT_PLATFORM, NEON_CSI_FX1_SUB_EVENT_WC_ALIGN_ERROR},
    {CSI_IRQ(FMTBRIDGE_OVERFLOW), NEON_CSI_EVENT_PLATFORM, NEON_CSI_FX1_SUB_EVENT_FORMAT_BRIDGE_OVERFLOW},
    {CSI_IRQ(FRAME_NUM_SEQ_ERR), NEON_CSI_EVENT_PLATFORM, NEON_CSI_FX1_SUB_EVENT_FRAME_NUMBER_ERROR},
};
#define CSI_IRQ_MAP_COUNT (sizeof(csi_irq_map) / sizeof(csi_irq_map[0]))

/*==================================================================================================
*                                  PRIVATE FUNCTION PROTOTYPES
==================================================================================================*/
static bool csi_metadata_dt_valid(uint8_t data_type);
static neon_status_t csi_dphy_bringup(neon_csi_handle_t *handle, bool test_pattern);
static neon_status_t csi_quiesce(void);
static void csi_write_receive_path(const neon_csi_config_t *config, const neon_csi_layout_t *layout);
static void csi_write_stream_path(const neon_csi_config_t *config);
static void csi_write_dma_path(const neon_csi_config_t *config, const neon_csi_layout_t *layout,
                               neon_csi_fx1_error_policy_t policy);
static void csi_write_tpg(neon_csi_raw_format_t raw_format, const neon_csi_fx1_tpg_config_t *tpg);
static uint32_t csi_irq_enable_mask(const neon_csi_config_t *config);
static neon_status_t csi_tpg_switch_on(void);
static neon_status_t csi_wait_sensor_source(void);
static neon_csi_buffer_state_t csi_buffer_state(uint32_t field);
static void csi_check_link(neon_csi_handle_t *handle);
static uint32_t csi_pending_sub_events(const neon_csi_handle_t *handle, uint32_t irq_status, neon_csi_event_t event,
                                       uint32_t *irq_bits);
static bool csi_frame_id_before(uint32_t a, uint32_t b);
static uint32_t csi_record_word(uint32_t meta_addr, uint32_t word);
static uint32_t csi_metadata_dt_config(uint8_t data_type, bool capture, bool fnum_check, bool extract);

/*==================================================================================================
*                                       GLOBAL FUNCTIONS
==================================================================================================*/
neon_status_t neon_csi_get_caps(neon_csi_caps_t *caps)
{
    if (caps == NULL) {
        return NEON_INVALID_PARAM;
    }

    *caps = fx1_caps;
    return NEON_OK;
}

neon_status_t neon_csi_init(neon_csi_handle_t *handle, neon_mipi_handle_t *mipi)
{
    if ((handle == NULL) || (mipi == NULL)) {
        return NEON_INVALID_PARAM;
    }

    /* MAS 9.1 step 1: refuse an IP this driver does not know. */
    if (fx1_read32(CSI_REG(IP_VERSION)) != CSI_EXPECTED_IP_VERSION) {
        return NEON_NOT_SUPPORTED;
    }

    *handle = (neon_csi_handle_t){0};
    handle->mipi = mipi;
    handle->caps = fx1_caps;
    handle->state = NEON_CSI_STATE_IDLE;
    return NEON_OK;
}

neon_status_t neon_csi_deinit(neon_csi_handle_t *handle)
{
    if (handle == NULL) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state == NEON_CSI_STATE_RUNNING) {
        return NEON_BUSY;
    }

    /* The hardware is left as it is: stop() already quieted it. */
    *handle = (neon_csi_handle_t){0};
    handle->state = NEON_CSI_STATE_RESET;
    return NEON_OK;
}

void neon_csi_get_default_config(neon_csi_config_t *config)
{
    if (config == NULL) {
        return;
    }

    *config = fx1_default_config;
}

neon_status_t neon_csi_calculate_layout(const neon_csi_config_t *config, neon_csi_layout_t *layout)
{
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_line;
    uint32_t pitch;
    uint32_t region;
    uint32_t meta;
    uint64_t buffer;

    if ((config == NULL) || (layout == NULL)) {
        return NEON_INVALID_PARAM;
    }
    if ((config->raw_format != NEON_CSI_RAW10) && (config->raw_format != NEON_CSI_RAW12)) {
        return NEON_NOT_SUPPORTED;
    }

    /* Geometry that reaches memory: the generator's when it is enabled. */
    if (config->test_pattern.enable) {
        width = (uint32_t)config->test_pattern.width & ~(CSI_TPG_WIDTH_STEP - 1U); /* rounded down, as the TPG does */
        height = config->test_pattern.height;                                     /* per virtual channel */
        if ((width < CSI_TPG_MIN_SIZE) || (height < CSI_TPG_MIN_SIZE)) {
            return NEON_INVALID_PARAM;
        }
    } else {
        width = config->width;
        height = config->height;
        if ((width == 0U) || (height == 0U)) {
            return NEON_INVALID_PARAM;
        }
    }

    /* LINE_PITCH.bytes_per_line (MAS 9.3): W x 5/4 for RAW10, W x 3/2 for RAW12, 2 x W for RAW16 output. */
    if (config->pixel_packing == NEON_CSI_PACKING_UNPACKED16) {
        bytes_per_line = 2U * width;
    } else if (config->pixel_packing == NEON_CSI_PACKING_NATIVE) {
        bytes_per_line = (config->raw_format == NEON_CSI_RAW10) ? ((width * 5U) + 3U) / 4U : ((width * 3U) + 1U) / 2U;
    } else {
        return NEON_INVALID_PARAM;
    }
    if (bytes_per_line > CSI_MAX_BYTES_PER_LINE) {
        return NEON_INVALID_PARAM;
    }

    /* Stride = bytes_per_line + line_padding: a multiple of 8, padding at most 255 (LINE_PITCH[23:16]). */
    if (config->dma.line_pitch_bytes == 0U) {
        pitch = (bytes_per_line + (CSI_DMA_ALIGN - 1U)) & ~(CSI_DMA_ALIGN - 1U);
    } else {
        pitch = config->dma.line_pitch_bytes;
        if ((pitch < bytes_per_line) || ((pitch % CSI_DMA_ALIGN) != 0U) ||
            ((pitch - bytes_per_line) > CSI_MAX_LINE_PADDING)) {
            return NEON_INVALID_PARAM;
        }
    }

    /* One RAW region per channel: dma.buffer_size_bytes, or exactly one frame when it is left at 0. */
    region = (config->dma.buffer_size_bytes == 0U) ? (pitch * height) : config->dma.buffer_size_bytes;
    if ((region < (pitch * height)) || ((region % CSI_DMA_ALIGN) != 0U)) {
        return NEON_INVALID_PARAM;
    }

    /* Metadata area per channel: record at 0x00, embedded payload from 0x80. */
    meta = 0U;
    if (config->metadata.enable) {
        meta = config->metadata.area_size_bytes;
        if ((meta < CSI_META_PAYLOAD_OFFSET) || ((meta % CSI_DMA_ALIGN) != 0U)) {
            return NEON_INVALID_PARAM;
        }
    }

    /* One buffer is 2 x (region + meta): the VC1 regions are reserved even with one exposure (MAS 9.3). */
    buffer = (uint64_t)NEON_CSI_FX1_VC_COUNT * ((uint64_t)region + (uint64_t)meta);
    if (buffer > UINT32_MAX) {
        return NEON_INVALID_PARAM;
    }

    layout->width = (uint16_t)width;
    layout->height = (uint16_t)height;
    layout->bytes_per_line = bytes_per_line;
    layout->line_pitch_bytes = pitch;
    layout->frame_bytes = pitch * height;
    layout->buffer_bytes = (uint32_t)buffer;
    /* The Format Bridge expands to RAW16 right-aligned (dma_format = 1). */
    layout->sample_align =
        (config->pixel_packing == NEON_CSI_PACKING_UNPACKED16) ? NEON_CSI_ALIGN_RIGHT : NEON_CSI_ALIGN_NONE;
    return NEON_OK;
}

neon_status_t neon_csi_configure(neon_csi_handle_t *handle, const neon_csi_config_t *config)
{
    neon_csi_layout_t layout;
    neon_csi_fx1_tpg_config_t tpg;
    neon_status_t status;
    uint32_t event;
    uint32_t i;

    /* Validate everything first: a rejected configuration leaves the hardware as it was. */
    if ((handle == NULL) || (config == NULL)) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state == NEON_CSI_STATE_RUNNING) {
        return NEON_BUSY;
    }
    if (handle->state == NEON_CSI_STATE_RESET) {
        return NEON_ERROR; /* neon_csi_init() not called. */
    }
    status = neon_csi_calculate_layout(config, &layout);
    if (status != NEON_OK) {
        return status;
    }
    for (i = 0U; i < NEON_CSI_FX1_BUFFER_COUNT; i++) {
        /* Both ping-pong buffers are always used: DMA_BUFFER0_ADDR and DMA_BUFFER1_ADDR. */
        if ((config->dma.base_addr[i] == 0U) || ((config->dma.base_addr[i] % CSI_DMA_ALIGN) != 0U)) {
            return NEON_INVALID_PARAM;
        }
    }
    if (config->metadata.enable && !csi_metadata_dt_valid(config->metadata.data_type)) {
        return NEON_INVALID_PARAM;
    }
    if (config->test_pattern.pattern > (uint8_t)NEON_CSI_FX1_TPG_HORIZONTAL_BARS) {
        return NEON_INVALID_PARAM; /* TPG_CONTROL.tpg_pattern is one bit. */
    }

    /* MAS 9.1 steps 2 to 6: D-PHY. The test pattern path needs steps 2 and 3 only (MAS 9.1, TPG note). */
    status = csi_dphy_bringup(handle, config->test_pattern.enable);
    if (status != NEON_OK) {
        return status;
    }

    /* Not in MAS 9.1: bring a receiver left running (warm restart, second configure) back to rest. */
    status = csi_quiesce();
    if (status != NEON_OK) {
        return status;
    }

    /* MAS 9.1 step 4, CSI part: PROTOCOL_CONFIG.vc_exposure_map = 0, VC0 is the Long (lead) channel.
     * ATTENTION: read-modify-write, active_lanes [1:0] of the same register belongs to the D-PHY driver.
     * It must be 0 while the test pattern generator is the source. neon_csi_fx1_set_lead_vc() changes it. */
    fx1_write32(CSI_REG(PROTOCOL_CONFIG),
                fx1_read32(CSI_REG(PROTOCOL_CONFIG)) & NEON_FX1_MIPI_CSI2_PROTOCOL_CONFIG_RW_MASK &
                    ~CSI_BIT(PROTOCOL_CONFIG, VC_EXPOSURE_MAP));

    csi_write_receive_path(config, &layout); /* MAS 9.1 step 7. */
    csi_write_stream_path(config);           /* MAS 9.1 step 8. */
    csi_write_dma_path(config, &layout, NEON_CSI_FX1_ERROR_POLICY_RELEASE_WITH_ERROR); /* MAS 9.1 step 9a. */

    /* MAS 9.1 TPG note: TPG_RESOLUTION, TPG_BLANKING and TPG_CONTROL, with tpg_enable left to start().
     * Written on the sensor path too, harmless while tpg_enable = 0. */
    tpg = fx1_default_tpg;
    tpg.base = config->test_pattern;
    csi_write_tpg(config->raw_format, &tpg);

    handle->config = *config;
    for (event = 0U; event < (uint32_t)NEON_CSI_EVENT_COUNT; event++) {
        /* Bits FX1 cannot raise are ignored, as for neon_csi_set_sub_event_mask(). */
        handle->config.sub_event_mask[event] &= fx1_caps.supported_sub_events[event];
    }
    handle->acquired_mask = 0U;
    handle->software_sub_events = 0U;
    /* A later change of either epoch means the link was reset under this configuration. */
    handle->phy_epoch = handle->mipi->phy_epoch;
    handle->config_epoch = handle->mipi->config_epoch;
    handle->state = NEON_CSI_STATE_CONFIGURED;
    return NEON_OK;
}

neon_status_t neon_csi_start(neon_csi_handle_t *handle)
{
    neon_status_t status;
    uint32_t dma_control;
    bool test_pattern;

    if (handle == NULL) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state == NEON_CSI_STATE_RUNNING) {
        return NEON_BUSY;
    }
    if (handle->state != NEON_CSI_STATE_CONFIGURED) {
        return NEON_ERROR;
    }
    /* NEON_MIPI_RECOVER_FULL (CORE_CONFIG.full_reset) returned every register to reset: configure again. */
    if (handle->mipi->config_epoch != handle->config_epoch) {
        handle->state = NEON_CSI_STATE_IDLE;
        return NEON_ERROR;
    }
    test_pattern = handle->config.test_pattern.enable;

    /* Sensor path after a TPG run: stop()/configure() cleared tpg_enable, but the mux returns to the sensor
     * only once no frame is in flight. ATTENTION: the sensor must not stream before active_src = 0
     * (CORE_STATUS.src_switch_pending), and the application starts it after start() returns. */
    if (!test_pattern) {
        status = csi_wait_sensor_source();
        if (status != NEON_OK) {
            return status;
        }
    }

    /* Sensor path: pin the link so it cannot be released while frames arrive. The TPG path never has a
     * READY link (MAS 9.1 skips steps 4 to 6), so it is not locked. */
    if (!test_pattern) {
        status = neon_mipi_lock(handle->mipi);
        if (status != NEON_OK) {
            return status; /* NEON_ERROR: the link is not READY, e.g. after a recovery. */
        }
        handle->link_locked = true;
    }

    /* MAS 9.1 step 9, second half: dma_enable, then dma_start. Read-modify-write keeps dma_format and
     * err_frame_policy from configure() or neon_csi_fx1_set_dma_config(). */
    dma_control = fx1_read32(CSI_REG(DMA_CONTROL)) & NEON_FX1_MIPI_CSI2_DMA_CONTROL_RW_MASK;
    dma_control |= CSI_BIT(DMA_CONTROL, DMA_ENABLE);
    fx1_write32(CSI_REG(DMA_CONTROL), dma_control);
    dma_control |= CSI_BIT(DMA_CONTROL, DMA_START);
    fx1_write32(CSI_REG(DMA_CONTROL), dma_control);

    /* MAS 9.1 step 10. */
    fx1_write32(CSI_REG(CORE_CONFIG), CSI_BIT(CORE_CONFIG, CORE_ENABLE));

    /* MAS 9.1 step 11 (= MAS 9.2 enabling sequence). ATTENTION: clear first, a flag already set would
     * assert csi2_irq the moment its enable is written. */
    fx1_write32(CSI_REG(IRQ_STATUS), NEON_FX1_MIPI_CSI2_IRQ_STATUS_W1C_MASK);
    fx1_write32(CSI_REG(IRQ_ENABLE), csi_irq_enable_mask(&handle->config));
    fx1_write32(CSI_REG(GLOBAL_IRQ_ENABLE), CSI_BIT(GLOBAL_IRQ_ENABLE, GLOBAL_ENABLE));

    /* MAS 9.1 TPG note, after step 11. TPG_RESOLUTION and TPG_BLANKING were written by configure(). */
    if (test_pattern) {
        status = csi_tpg_switch_on();
        if (status != NEON_OK) {
            /* Rollback: back to CONFIGURED with the configuration intact, start() may be called again. */
            (void)csi_quiesce();
            return status;
        }
    }

    handle->state = NEON_CSI_STATE_RUNNING;
    return NEON_OK;
}

neon_status_t neon_csi_stop(neon_csi_handle_t *handle)
{
    neon_status_t status;

    if (handle == NULL) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state != NEON_CSI_STATE_RUNNING) {
        return NEON_ERROR;
    }

    /* IRQ off, TPG off, DMA aborted and drained, core disabled, IRQ_STATUS cleared.
     * ATTENTION: the application stops the sensor first; a frame still arriving is dropped. */
    status = csi_quiesce();
    if (status != NEON_OK) {
        /* AXI writes still outstanding: stay RUNNING and keep the link, stop() may be called again. */
        return status;
    }

    /* Buffers held by software keep SW_OWNED through dma_abort and would never be claimed again:
     * hand both back. A release of a buffer that is not SW_OWNED is ignored by the hardware. */
    fx1_write32(CSI_REG(BUFFER_RELEASE),
                CSI_BIT(BUFFER_RELEASE, RELEASE_BUF0) | CSI_BIT(BUFFER_RELEASE, RELEASE_BUF1));
    handle->acquired_mask = 0U;

    if (handle->link_locked) {
        (void)neon_mipi_unlock(handle->mipi);
        handle->link_locked = false;
    }

    /* Configuration registers are kept: start() may follow without configure(). */
    handle->state = NEON_CSI_STATE_CONFIGURED;
    return NEON_OK;
}

neon_status_t neon_csi_recover(neon_csi_handle_t *handle)
{
    neon_mipi_status_t link;
    neon_status_t status;
    uint32_t polls;

    if (handle == NULL) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state != NEON_CSI_STATE_RUNNING) {
        return NEON_ERROR;
    }

    /* ATTENTION: soft_reset is synchronised into rxbyteclkhs. Without the byte clock it never reaches
     * Lane Management and does nothing; the TPG path has no byte clock either. The caller escalates to
     * neon_mipi_recover(NEON_MIPI_RECOVER_FULL), then configure() again. */
    status = neon_mipi_get_status(handle->mipi, &link);
    if (status != NEON_OK) {
        return status;
    }
    if (!link.clock_hs_active) {
        return NEON_ERROR;
    }

    /* CORE_CONFIG.soft_reset is a level (RW), not self-clearing: assert, release, then wait. Configuration
     * registers are kept, core_enable stays 1. Flushes the FIFOs, returns every state machine to idle and
     * clears IRQ_STATUS, the counters and the sticky flags. */
    fx1_write32(CSI_REG(CORE_CONFIG), CSI_BIT(CORE_CONFIG, CORE_ENABLE) | CSI_BIT(CORE_CONFIG, SOFT_RESET));
    fx1_write32(CSI_REG(CORE_CONFIG), CSI_BIT(CORE_CONFIG, CORE_ENABLE));
    for (polls = 0U; (fx1_read32(CSI_REG(CORE_STATUS)) & CSI_BIT(CORE_STATUS, RESET_IN_PROGRESS)) != 0U; polls++) {
        if (polls >= CSI_RESET_POLLS) {
            return NEON_TIMEOUT;
        }
    }

    /* A buffer owned by software keeps its state through soft_reset: hand both back, as documented. */
    fx1_write32(CSI_REG(BUFFER_RELEASE),
                CSI_BIT(BUFFER_RELEASE, RELEASE_BUF0) | CSI_BIT(BUFFER_RELEASE, RELEASE_BUF1));
    handle->acquired_mask = 0U;

    /* ATTENTION: every soft_reset leaves one spurious LANE_ALIGN_ABORT, and events during the reset and
     * the RST_TAIL (256 sys_clk) after it are not recorded at all. Clear the spurious flag. */
    fx1_write32(CSI_REG(IRQ_STATUS), NEON_FX1_MIPI_CSI2_IRQ_STATUS_W1C_MASK);
    return NEON_OK;
}

neon_status_t neon_csi_get_status(neon_csi_handle_t *handle, neon_csi_status_t *status)
{
    uint32_t ownership;
    uint32_t ecc;
    uint32_t i;

    if ((handle == NULL) || (status == NULL)) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state == NEON_CSI_STATE_RESET) {
        return NEON_ERROR; /* neon_csi_init() not called. */
    }

    *status = (neon_csi_status_t){0};
    status->state = handle->state;

    /* ATTENTION: right after BUFFER_RELEASE the old state may still read back (clock domain crossing).
     * Entries beyond the FX1 ping-pong pair stay NEON_CSI_BUFFER_HW_OWNED (0). */
    ownership = fx1_read32(CSI_REG(BUFFER_OWNERSHIP));
    status->buffer_state[0] = csi_buffer_state(CSI_GET(BUFFER_OWNERSHIP, BUF0_STATE, ownership));
    status->buffer_state[1] = csi_buffer_state(CSI_GET(BUFFER_OWNERSHIP, BUF1_STATE, ownership));
    for (i = NEON_CSI_FX1_BUFFER_COUNT; i < NEON_CSI_MAX_BUFFERS; i++) {
        status->buffer_state[i] = NEON_CSI_BUFFER_HW_OWNED;
    }

    /* ATTENTION: every counter is cleared by soft_reset (neon_csi_recover()) and by a full reset.
     * PACKET_COUNT and FRAME_COUNT wrap; the error counters saturate (ECC halves at 0xFFFF each). */
    status->packet_count = fx1_read32(CSI_REG(PACKET_COUNT));
    status->frame_count = fx1_read32(CSI_REG(FRAME_COUNT));
    ecc = fx1_read32(CSI_REG(ECC_ERROR_COUNT));
    status->ecc_error_count =
        CSI_GET(ECC_ERROR_COUNT, ECC_1BIT_COUNT, ecc) + CSI_GET(ECC_ERROR_COUNT, ECC_2BIT_COUNT, ecc);
    status->crc_error_count = fx1_read32(CSI_REG(CRC_ERROR_COUNT));
    /* Every dropped capture, on the stream path, the DMA path or both. */
    status->frame_drop_count = fx1_read32(CSI_REG(FRAME_DROP_COUNT));
    return NEON_OK;
}

neon_status_t neon_csi_fx1_get_status(neon_csi_handle_t *handle, neon_csi_fx1_status_t *status)
{
    neon_status_t result;
    uint32_t axi;

    if ((handle == NULL) || (status == NULL)) {
        return NEON_INVALID_PARAM;
    }
    result = neon_csi_get_status(handle, &status->base);
    if (result != NEON_OK) {
        return result;
    }

    /* 16-bit counters, saturating at 0xFFFF, cleared by soft_reset. */
    status->lane_abort_count =
        (uint16_t)CSI_GET(LANE_ABORT_COUNT, LANE_ABORT_COUNT, fx1_read32(CSI_REG(LANE_ABORT_COUNT)));
    axi = fx1_read32(CSI_REG(AXI_ERROR_COUNT));
    status->axi_slverr_count = (uint16_t)CSI_GET(AXI_ERROR_COUNT, BRESP_SLVERR_COUNT, axi);
    status->axi_decerr_count = (uint16_t)CSI_GET(AXI_ERROR_COUNT, BRESP_DECERR_COUNT, axi);
    return NEON_OK;
}

neon_status_t neon_csi_get_events(neon_csi_handle_t *handle, uint32_t *events)
{
    uint32_t irq_status;
    uint32_t irq_bits;
    uint32_t event;

    if ((handle == NULL) || (events == NULL)) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state == NEON_CSI_STATE_RESET) {
        return NEON_ERROR;
    }
    csi_check_link(handle);

    *events = 0U;
    irq_status = fx1_read32(CSI_REG(IRQ_STATUS));
    for (event = 0U; event < (uint32_t)NEON_CSI_EVENT_COUNT; event++) {
        if (csi_pending_sub_events(handle, irq_status, (neon_csi_event_t)event, &irq_bits) != 0U) {
            *events |= NEON_CSI_EVENT_BIT(event);
        }
    }
    return NEON_OK;
}

neon_status_t neon_csi_take_sub_events(neon_csi_handle_t *handle, neon_csi_event_t event, uint32_t *sub_events)
{
    uint32_t irq_bits;

    if ((handle == NULL) || (sub_events == NULL) || ((uint32_t)event >= (uint32_t)NEON_CSI_EVENT_COUNT)) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state == NEON_CSI_STATE_RESET) {
        return NEON_ERROR;
    }
    csi_check_link(handle);

    /* MAS 9.2: read IRQ_STATUS, write back exactly the bits read (RW1C). A flag raised after the read, or in
     * the same cycle as its clear, stays set for the next call. Bits of other events are not touched.
     * ATTENTION: the write completes only once those bits read back 0 (up to 16 lite_aclk). */
    *sub_events = csi_pending_sub_events(handle, fx1_read32(CSI_REG(IRQ_STATUS)), event, &irq_bits);
    if (irq_bits != 0U) {
        fx1_write32(CSI_REG(IRQ_STATUS), irq_bits);
    }
    if (event == NEON_CSI_EVENT_SYSTEM) {
        handle->software_sub_events = 0U;
    }
    return NEON_OK;
}

neon_status_t neon_csi_get_sub_events(neon_csi_handle_t *handle, neon_csi_event_t event, uint32_t *sub_events)
{
    uint32_t irq_bits;

    if ((handle == NULL) || (sub_events == NULL) || ((uint32_t)event >= (uint32_t)NEON_CSI_EVENT_COUNT)) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state == NEON_CSI_STATE_RESET) {
        return NEON_ERROR;
    }
    csi_check_link(handle);

    *sub_events = csi_pending_sub_events(handle, fx1_read32(CSI_REG(IRQ_STATUS)), event, &irq_bits);
    return NEON_OK;
}

neon_status_t neon_csi_set_sub_event_mask(neon_csi_handle_t *handle, neon_csi_event_t event, uint32_t mask)
{
    if ((handle == NULL) || ((uint32_t)event >= (uint32_t)NEON_CSI_EVENT_COUNT)) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state == NEON_CSI_STATE_RESET) {
        return NEON_ERROR;
    }

    handle->config.sub_event_mask[event] = mask & fx1_caps.supported_sub_events[event];
    /* While running the new mask takes effect at once; otherwise start() writes it. Masking a source does
     * not stop its IRQ_STATUS flag from being set, so its sub event can still be read. */
    if (handle->state == NEON_CSI_STATE_RUNNING) {
        fx1_write32(CSI_REG(IRQ_ENABLE), csi_irq_enable_mask(&handle->config));
    }
    return NEON_OK;
}

neon_status_t neon_csi_acquire_frame(neon_csi_handle_t *handle, neon_csi_frame_t *frame)
{
    uint32_t ownership;
    uint32_t frame_ids;
    uint32_t id[NEON_CSI_FX1_BUFFER_COUNT];
    uint32_t state[NEON_CSI_FX1_BUFFER_COUNT];
    uint32_t region;
    uint32_t meta;
    uint32_t base;
    uint32_t lead;
    uint32_t word;
    uint32_t last;
    uint32_t vc;
    int32_t pick;
    uint32_t b;

    if ((handle == NULL) || (frame == NULL)) {
        return NEON_INVALID_PARAM;
    }
    csi_check_link(handle);
    if (handle->state != NEON_CSI_STATE_RUNNING) {
        return NEON_ERROR;
    }

    /* MAS 9.3 buffer service, step 1: every buffer in SW_OWNED_VALID or SW_OWNED_ERROR is owned by software.
     * One buf_done interrupt can announce both buffers. */
    ownership = fx1_read32(CSI_REG(BUFFER_OWNERSHIP));
    frame_ids = fx1_read32(CSI_REG(FRAME_ID));
    state[0] = CSI_GET(BUFFER_OWNERSHIP, BUF0_STATE, ownership);
    state[1] = CSI_GET(BUFFER_OWNERSHIP, BUF1_STATE, ownership);
    id[0] = CSI_GET(FRAME_ID, FRAME_ID_BUF0, frame_ids);
    id[1] = CSI_GET(FRAME_ID, FRAME_ID_BUF1, frame_ids);

    /* Step 2: of the completed buffers software does not hold yet, the oldest frame_id first. */
    pick = -1;
    for (b = 0U; b < NEON_CSI_FX1_BUFFER_COUNT; b++) {
        if ((csi_buffer_state(state[b]) == NEON_CSI_BUFFER_HW_OWNED) || ((handle->acquired_mask & (1U << b)) != 0U)) {
            continue;
        }
        if ((pick < 0) || csi_frame_id_before(id[b], id[(uint32_t)pick])) {
            pick = (int32_t)b;
        }
    }
    if (pick < 0) {
        return NEON_BUSY;
    }
    b = (uint32_t)pick;

    /* Layout of buffer b (MAS 9.3 capture flow, step 2): RAW VC0, RAW VC1, metadata VC0, metadata VC1. */
    region = fx1_read32(CSI_REG(DMA_BUFFER_SIZE)); /* As programmed, also when config left it at 0. */
    meta = handle->config.metadata.enable ? handle->config.metadata.area_size_bytes : 0U;
    base = handle->config.dma.base_addr[b];

    *frame = (neon_csi_frame_t){0};
    frame->buffer_index = (uint8_t)b;
    frame->state = csi_buffer_state(state[b]);
    frame->base_addr = base;
    frame->frame_id = id[b];
    frame->last_valid_line = UINT32_MAX;
    for (vc = 0U; vc < NEON_CSI_FX1_VC_COUNT; vc++) {
        frame->pixel_addr[vc] = base + (vc * region);
        frame->meta_addr[vc] = (meta != 0U) ? (base + (NEON_CSI_FX1_VC_COUNT * region) + (vc * meta)) : 0U;
    }

    /* Step 4: SW_OWNED_ERROR holds part of the capture. 0xFFFF: no line is valid. */
    if (frame->state == NEON_CSI_BUFFER_SW_ERROR) {
        word = fx1_read32(CSI_REG(BUFFER_ERROR_INFO));
        last = (b == 0U) ? CSI_GET(BUFFER_ERROR_INFO, BUF0_LAST_VALID_LINE, word)
                         : CSI_GET(BUFFER_ERROR_INFO, BUF1_LAST_VALID_LINE, word);
        frame->last_valid_line = (last == 0xFFFFU) ? UINT32_MAX : last;
    }

    /* Lead channel: VC0, or VC1 when vc_exposure_map = 1. Always VC0 with the test pattern generator. */
    lead = ((fx1_read32(CSI_REG(PROTOCOL_CONFIG)) & CSI_BIT(PROTOCOL_CONFIG, VC_EXPOSURE_MAP)) != 0U) ? 1U : 0U;

    if (meta != 0U) {
        /* Step 3: a channel took part when its record carries the frame_id of the buffer.
         * ATTENTION: the records are in DDR, written by the DMA: invalidate the CPU cache before reading.
         * The pixels are the consumer's to invalidate. On SW_OWNED_ERROR the records may be incomplete. */
        for (vc = 0U; vc < NEON_CSI_FX1_VC_COUNT; vc++) {
            fx1_dma_complete((const volatile void *)(uintptr_t)frame->meta_addr[vc], CSI_RECORD_BYTES,
                             FX1_DMA_FROM_DEVICE);
            if ((csi_record_word(frame->meta_addr[vc], CSI_RECORD_WORD_FRAME) & CSI_FRAME_ID_PERIOD) != id[b]) {
                continue;
            }
            frame->vc_mask |= (uint8_t)(1U << vc);
            word = csi_record_word(frame->meta_addr[vc], CSI_RECORD_WORD_GEOM);
            frame->vc[vc].byte_count = (uint16_t)(word & 0xFFFFU);
            frame->vc[vc].line_count = (uint16_t)(word >> 16);
            frame->vc[vc].data_type = (uint8_t)((csi_record_word(frame->meta_addr[vc], CSI_RECORD_WORD_EMB) >> 24) & 0x3FU);
            frame->vc[vc].error = ((csi_record_word(frame->meta_addr[vc], CSI_RECORD_WORD_INFO) >> 16) & 0x1FU) != 0U;
        }
        /* Both records of a capture carry the same timestamp; take the lead's, or the one that joined. */
        vc = ((frame->vc_mask & (1U << lead)) != 0U) ? lead : (1U - lead);
        if ((frame->vc_mask & (1U << vc)) != 0U) {
            frame->timestamp_ns = ((uint64_t)csi_record_word(frame->meta_addr[vc], CSI_RECORD_WORD_TS_HI) << 32) |
                                  (uint64_t)csi_record_word(frame->meta_addr[vc], CSI_RECORD_WORD_TS_LO);
        }
    } else {
        /* No record: the registers hold the LAST PUBLISHED capture only. ATTENTION: when both buffers are
         * owned by software the older one gets the newer capture's timestamp and line counts. */
        word = fx1_read32(CSI_REG(TIMESTAMP_LO)); /* LO first: it latches HI into a shadow. */
        frame->timestamp_ns = ((uint64_t)fx1_read32(CSI_REG(TIMESTAMP_HI)) << 32) | (uint64_t)word;
        if (handle->config.test_pattern.enable) {
            /* The TPG accumulates no geometry. VC1 joins only in dual-VC mode. */
            frame->vc_mask = ((fx1_read32(CSI_REG(TPG_CONTROL)) & CSI_BIT(TPG_CONTROL, TPG_DUAL_VC_EN)) != 0U) ? 3U : 1U;
        } else {
            word = fx1_read32(CSI_REG(VC0_IMAGE_INFO1));
            frame->vc[0].byte_count = (uint16_t)CSI_GET(VC0_IMAGE_INFO1, BYTE_COUNT, word);
            frame->vc[0].line_count = (uint16_t)CSI_GET(VC0_IMAGE_INFO1, LINE_COUNT, word);
            frame->vc[0].data_type = (uint8_t)CSI_GET(VC0_IMAGE_INFO2, DATA_TYPE, fx1_read32(CSI_REG(VC0_IMAGE_INFO2)));
            word = fx1_read32(CSI_REG(VC1_IMAGE_INFO1));
            frame->vc[1].byte_count = (uint16_t)CSI_GET(VC1_IMAGE_INFO1, BYTE_COUNT, word);
            frame->vc[1].line_count = (uint16_t)CSI_GET(VC1_IMAGE_INFO1, LINE_COUNT, word);
            frame->vc[1].data_type = (uint8_t)CSI_GET(VC1_IMAGE_INFO2, DATA_TYPE, fx1_read32(CSI_REG(VC1_IMAGE_INFO2)));
            for (vc = 0U; vc < NEON_CSI_FX1_VC_COUNT; vc++) {
                if (frame->vc[vc].line_count != 0U) {
                    frame->vc_mask |= (uint8_t)(1U << vc);
                }
            }
        }
    }

    handle->acquired_mask |= (uint8_t)(1U << b);
    return NEON_OK;
}

neon_status_t neon_csi_release_frame(neon_csi_handle_t *handle, uint8_t buffer_index)
{
    if (handle == NULL) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state != NEON_CSI_STATE_RUNNING) {
        return NEON_ERROR;
    }
    if ((buffer_index >= NEON_CSI_FX1_BUFFER_COUNT) || ((handle->acquired_mask & (1U << buffer_index)) == 0U)) {
        return NEON_INVALID_PARAM;
    }

    /* MAS 9.3 buffer service, step 6. ATTENTION: return it before the lead Frame Start of the second capture
     * after its own, or that capture finds no free buffer and is dropped on the DMA path. BUFFER_OWNERSHIP
     * may still read the old state right after this write. */
    fx1_write32(CSI_REG(BUFFER_RELEASE), 1UL << buffer_index);
    handle->acquired_mask &= (uint8_t)~(1U << buffer_index);
    return NEON_OK;
}

neon_status_t neon_csi_read_short_packet(neon_csi_handle_t *handle, neon_csi_short_packet_t *packet)
{
    uint32_t word;

    if ((handle == NULL) || (packet == NULL)) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state != NEON_CSI_STATE_RUNNING) {
        return NEON_ERROR;
    }

    /* ATTENTION: the read pops the FIFO; data_type 0 means it was empty and nothing was removed.
     * No interrupt announces a short packet on FX1 (CORE_STATUS.sp_fifo_not_empty can be polled). */
    word = fx1_read32(CSI_REG(GENERIC_SHORT_PACKET));
    if (CSI_GET(GENERIC_SHORT_PACKET, DATA_TYPE, word) == CSI_SHORT_PACKET_EMPTY) {
        return NEON_BUSY;
    }
    packet->data = (uint16_t)CSI_GET(GENERIC_SHORT_PACKET, DATA, word);
    packet->data_type = (uint8_t)CSI_GET(GENERIC_SHORT_PACKET, DATA_TYPE, word);
    packet->virtual_channel = (uint8_t)CSI_GET(GENERIC_SHORT_PACKET, VIRTUAL_CHANNEL, word);
    return NEON_OK;
}

neon_status_t neon_csi_fx1_set_dma_config(neon_csi_handle_t *handle, const neon_csi_fx1_dma_config_t *dma)
{
    neon_csi_config_t config;
    neon_csi_layout_t layout;
    neon_status_t status;
    uint32_t i;

    if ((handle == NULL) || (dma == NULL)) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state == NEON_CSI_STATE_RUNNING) {
        return NEON_BUSY;
    }
    if (handle->state != NEON_CSI_STATE_CONFIGURED) {
        return NEON_ERROR;
    }
    if ((dma->error_policy != NEON_CSI_FX1_ERROR_POLICY_DISCARD) &&
        (dma->error_policy != NEON_CSI_FX1_ERROR_POLICY_RELEASE_WITH_ERROR)) {
        return NEON_INVALID_PARAM;
    }

    /* Same checks as configure() with the new DMA settings in place. */
    config = handle->config;
    config.dma = dma->base;
    status = neon_csi_calculate_layout(&config, &layout);
    if (status != NEON_OK) {
        return status;
    }
    for (i = 0U; i < NEON_CSI_FX1_BUFFER_COUNT; i++) {
        if ((config.dma.base_addr[i] == 0U) || ((config.dma.base_addr[i] % CSI_DMA_ALIGN) != 0U)) {
            return NEON_INVALID_PARAM;
        }
    }

    /* dma_enable is 0 since configure(): DMA_CONTROL.dma_format may be written. */
    csi_write_dma_path(&config, &layout, dma->error_policy);
    handle->config.dma = dma->base;
    return NEON_OK;
}

neon_status_t neon_csi_fx1_set_record_id(neon_csi_handle_t *handle, uint32_t record_id)
{
    if (handle == NULL) {
        return NEON_INVALID_PARAM;
    }
    if ((handle->state != NEON_CSI_STATE_CONFIGURED) && (handle->state != NEON_CSI_STATE_RUNNING)) {
        return NEON_ERROR;
    }

    /* Sampled when a capture opens: allowed while running, an open capture keeps the previous value. */
    fx1_write32(CSI_REG(METADATA_RECORD_ID), record_id);
    return NEON_OK;
}

neon_status_t neon_csi_fx1_set_lead_vc(neon_csi_handle_t *handle, uint8_t vc)
{
    uint32_t protocol;

    if ((handle == NULL) || (vc >= NEON_CSI_FX1_VC_COUNT)) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state == NEON_CSI_STATE_RUNNING) {
        return NEON_BUSY;
    }
    if (handle->state != NEON_CSI_STATE_CONFIGURED) {
        return NEON_ERROR;
    }
    if (handle->config.test_pattern.enable && (vc != 0U)) {
        return NEON_INVALID_PARAM; /* vc_exposure_map shall be 0 while the TPG is selected. */
    }

    /* ATTENTION: read-modify-write, active_lanes [1:0] of the same register belongs to the D-PHY driver. */
    protocol = fx1_read32(CSI_REG(PROTOCOL_CONFIG)) & NEON_FX1_MIPI_CSI2_PROTOCOL_CONFIG_RW_MASK &
               ~CSI_BIT(PROTOCOL_CONFIG, VC_EXPOSURE_MAP);
    fx1_write32(CSI_REG(PROTOCOL_CONFIG), protocol | CSI_FIELD(PROTOCOL_CONFIG, VC_EXPOSURE_MAP, vc));
    return NEON_OK;
}

neon_status_t neon_csi_fx1_set_metadata_config(neon_csi_handle_t *handle,
                                               const neon_csi_fx1_metadata_config_t *metadata)
{
    const neon_csi_fx1_extract_slot_t *slot;
    uint32_t cfg[NEON_CSI_FX1_EXTRACT_SLOTS];
    uint32_t i;

    if ((handle == NULL) || (metadata == NULL)) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state == NEON_CSI_STATE_RUNNING) {
        return NEON_BUSY;
    }
    if (handle->state != NEON_CSI_STATE_CONFIGURED) {
        return NEON_ERROR;
    }
    /* The buffers were sized by configure(): enable and area_size_bytes stay fixed, data_type may change. */
    if ((metadata->base.enable != handle->config.metadata.enable) ||
        (metadata->base.area_size_bytes != handle->config.metadata.area_size_bytes)) {
        return NEON_INVALID_PARAM;
    }
    if (metadata->base.enable && !csi_metadata_dt_valid(metadata->base.data_type)) {
        return NEON_INVALID_PARAM;
    }
    /* emb_extract_en is effective only while metadata_capture_en = 1. */
    if (metadata->extract_enable && !metadata->base.enable) {
        return NEON_INVALID_PARAM;
    }

    for (i = 0U; i < NEON_CSI_FX1_EXTRACT_SLOTS; i++) {
        slot = &metadata->extract[i];
        cfg[i] = 0U; /* len 0 disables the slot. */
        if (metadata->extract_enable) {
            if ((slot->length_bytes == 0U) || (slot->length_bytes > CSI_EXTRACT_MAX_LEN)) {
                return NEON_INVALID_PARAM;
            }
            cfg[i] = CSI_FIELD(EMB_EXTRACT_CFG0, LEN, slot->length_bytes) |
                     CSI_FIELD(EMB_EXTRACT_CFG0, BYTE_OFFSET, slot->byte_offset) |
                     CSI_FIELD(EMB_EXTRACT_CFG0, LINE_IDX, slot->line_index);
        }
    }

    /* MAS 9.1 step 7: EMB_EXTRACT_CFG0/1 before emb_extract_en. CFG1 has the same layout as CFG0. */
    fx1_write32(CSI_REG(EMB_EXTRACT_CFG0), cfg[0]);
    fx1_write32(CSI_REG(EMB_EXTRACT_CFG1), cfg[1]);
    fx1_write32(CSI_REG(METADATA_DT_CONFIG),
                csi_metadata_dt_config(metadata->base.enable ? metadata->base.data_type : (uint8_t)CSI_DEFAULT_METADATA_DT,
                                       metadata->base.enable, metadata->frame_number_check, metadata->extract_enable));
    handle->config.metadata.data_type = metadata->base.data_type;
    return NEON_OK;
}

neon_status_t neon_csi_fx1_get_extracted(const neon_csi_frame_t *frame, uint8_t vc,
                                         uint32_t values[NEON_CSI_FX1_EXTRACT_SLOTS])
{
    uint32_t i;

    if ((frame == NULL) || (values == NULL) || (vc >= NEON_CSI_FX1_VC_COUNT)) {
        return NEON_INVALID_PARAM;
    }
    if (((frame->vc_mask & (1U << vc)) == 0U) || (frame->meta_addr[vc] == 0U)) {
        return NEON_INVALID_PARAM;
    }

    /* Words 7 and 8 of the record: per frame, unlike EMB_EXTRACT_VCn_VALm which hold the last published one.
     * The cache over the record was invalidated by neon_csi_acquire_frame(). */
    for (i = 0U; i < NEON_CSI_FX1_EXTRACT_SLOTS; i++) {
        values[i] = csi_record_word(frame->meta_addr[vc], CSI_RECORD_WORD_EXTRACT + i);
    }
    return NEON_OK;
}

neon_status_t neon_csi_fx1_set_tpg_config(neon_csi_handle_t *handle, const neon_csi_fx1_tpg_config_t *tpg)
{
    const neon_csi_tpg_config_t *current;

    if ((handle == NULL) || (tpg == NULL)) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state == NEON_CSI_STATE_RUNNING) {
        return NEON_BUSY;
    }
    if (handle->state != NEON_CSI_STATE_CONFIGURED) {
        return NEON_ERROR;
    }
    /* The buffers were sized by configure() from config.test_pattern: enable and geometry stay fixed. */
    current = &handle->config.test_pattern;
    if ((tpg->base.enable != current->enable) || (tpg->base.width != current->width) ||
        (tpg->base.height != current->height)) {
        return NEON_INVALID_PARAM;
    }
    if ((tpg->base.pattern > (uint8_t)NEON_CSI_FX1_TPG_HORIZONTAL_BARS) || ((uint32_t)tpg->cfa_order > 3U)) {
        return NEON_INVALID_PARAM;
    }

    csi_write_tpg(handle->config.raw_format, tpg);
    handle->config.test_pattern.pattern = tpg->base.pattern;
    return NEON_OK;
}

/*==================================================================================================
*                                       PRIVATE FUNCTIONS
==================================================================================================*/
/* METADATA_DT_CONFIG.metadata_dt is 6 bits and shall not select an image Data Type the receiver also
 * parses (0x10, 0x11, 0x2B, 0x2C): CORE_STATUS.dt_conflict would be raised. */
static bool csi_metadata_dt_valid(uint8_t data_type)
{
    return (data_type <= 0x3FU) && (data_type != 0x10U) && (data_type != 0x11U) && (data_type != 0x2BU) &&
           (data_type != 0x2CU);
}

/*
 * MAS 9.1 steps 2 to 6: D-PHY bring-up.
 * PLACEHOLDER: these steps belong to the D-PHY driver (neon_mipi). The application runs
 * neon_mipi_acquire() before neon_csi_configure(); this helper marks where each step sits and only
 * confirms the result. neon_mipi_wait_ready() returns at once when the link is already READY.
 * ATTENTION: on the test pattern path MAS 9.1 skips steps 4 to 6 only. Steps 2 and 3 (D-PHY reset)
 * are still required, so the link must be acquired; it never becomes READY without a sensor, which is
 * why start() takes neon_mipi_lock() on the sensor path only.
 * TODO(dphy): FX1 has only the simulation backend of neon_mipi. The register backend
 * (PHY_CONFIG, PROTOCOL_CONFIG.active_lanes, PHY_STATUS, LANE_STATUS) is not written yet.
 */
static neon_status_t csi_dphy_bringup(neon_csi_handle_t *handle, bool test_pattern)
{
    /* Step 2: PHY_CONFIG.dphy_rst_req = 1                          -> neon_mipi_acquire()    */
    /* Step 3: poll CORE_STATUS.reset_in_progress until 0           -> neon_mipi_acquire()    */
    if (handle->mipi->state == NEON_MIPI_STATE_RELEASED) {
        return NEON_ERROR; /* Steps 2 and 3 are required on both paths. */
    }
    if (test_pattern) {
        return NEON_OK; /* Steps 4 to 6 are not required (MAS 9.1, TPG note). */
    }
    /* Step 4: PROTOCOL_CONFIG.active_lanes (vc_exposure_map: here) -> neon_mipi_acquire()    */
    /* Step 5: PHY_CONFIG.phy_enable, cl_enable, dl_enable          -> neon_mipi_acquire()    */
    /* Step 6: poll PHY_STATUS.cl_stopstate, laneX_stopstate        -> neon_mipi_wait_ready() */
    /* NEON_ERROR: link not acquired. NEON_TIMEOUT: no Stop State yet, the sensor may not drive LP-11. */
    return neon_mipi_wait_ready(handle->mipi, CSI_DPHY_READY_POLLS);
}

/*
 * Not in MAS 9.1, which starts from reset. Brings a receiver that may still be running back to the
 * state 9.1 expects before step 7. ATTENTION, order matters:
 * - IRQ off first, so nothing fires while the rest is changed.
 * - tpg_enable = 0, so configure() may reprogram the generator (its size must stay >= 8 while enabled).
 *   The other TPG_CONTROL bits are kept: a start() rollback must not lose neon_csi_fx1_set_tpg_config().
 * - dma_abort and wait for dma_busy = 0: AXI bursts already issued must complete.
 * - Clear dma_start before dma_enable, abort last. dma_format may change only after this.
 * - core_enable = 0 (reset value 1): drops any open capture, one record per channel marked dropped.
 * - IRQ_STATUS cleared last, so start() does not see the flags raised by this sequence.
 * On NEON_TIMEOUT dma_abort is left set: no buffer is claimed until the next configure.
 */
static neon_status_t csi_quiesce(void)
{
    uint32_t dma_control;
    uint32_t polls;

    fx1_write32(CSI_REG(GLOBAL_IRQ_ENABLE), 0U);
    fx1_write32(CSI_REG(IRQ_ENABLE), 0U);

    /* tpg_enable = 0: the generator ends its frame and the mux returns to the sensor once no frame is in
     * flight (CORE_STATUS.active_src); not waited for here, start() waits for its own switch. */
    fx1_write32(CSI_REG(TPG_CONTROL), fx1_read32(CSI_REG(TPG_CONTROL)) & NEON_FX1_MIPI_CSI2_TPG_CONTROL_RW_MASK &
                                          ~CSI_BIT(TPG_CONTROL, TPG_ENABLE));

    dma_control = fx1_read32(CSI_REG(DMA_CONTROL)) & NEON_FX1_MIPI_CSI2_DMA_CONTROL_RW_MASK;
    dma_control |= CSI_BIT(DMA_CONTROL, DMA_ABORT);
    fx1_write32(CSI_REG(DMA_CONTROL), dma_control);
    for (polls = 0U; (fx1_read32(CSI_REG(DMA_STATUS)) & CSI_BIT(DMA_STATUS, DMA_BUSY)) != 0U; polls++) {
        if (polls >= CSI_DMA_IDLE_POLLS) {
            return NEON_TIMEOUT;
        }
    }
    dma_control &= ~CSI_BIT(DMA_CONTROL, DMA_START);
    fx1_write32(CSI_REG(DMA_CONTROL), dma_control);
    dma_control &= ~CSI_BIT(DMA_CONTROL, DMA_ENABLE);
    fx1_write32(CSI_REG(DMA_CONTROL), dma_control);
    dma_control &= ~CSI_BIT(DMA_CONTROL, DMA_ABORT);
    fx1_write32(CSI_REG(DMA_CONTROL), dma_control);

    /* soft_reset stays 0 and full_reset is write-only: 0 clears core_enable alone. */
    fx1_write32(CSI_REG(CORE_CONFIG), 0U);

    /* The write completes only once the bits read back as 0. */
    fx1_write32(CSI_REG(IRQ_STATUS), NEON_FX1_MIPI_CSI2_IRQ_STATUS_W1C_MASK);
    return NEON_OK;
}

/*
 * MAS 9.1 step 7: METADATA_DT_CONFIG, EXPECTED_RESOLUTION, METADATA_RECORD_ID.
 * ATTENTION: fnum_check_en and emb_extract_en are written 0; neon_csi_fx1_set_metadata_config()
 * enables them between configure() and start(). emb_extract_en only once the field offsets are known
 * (MAS 9.1): until then the embedded lines still reach DDR and the offsets can be read there.
 * METADATA_RECORD_ID is written 0; neon_csi_fx1_set_record_id() sets another value.
 */
static void csi_write_receive_path(const neon_csi_config_t *config, const neon_csi_layout_t *layout)
{
    uint32_t metadata_dt_config;

    if (config->metadata.enable) {
        metadata_dt_config = csi_metadata_dt_config(config->metadata.data_type, true, false, false);
    } else {
        /* Disabled: keep the reset Data Type, a 0 left in the config is not a meaningful choice. */
        metadata_dt_config = csi_metadata_dt_config(CSI_DEFAULT_METADATA_DT, false, false, false);
    }
    fx1_write32(CSI_REG(METADATA_DT_CONFIG), metadata_dt_config);

    /* Reference of the resolution check. The check is not run on test pattern frames. */
    fx1_write32(CSI_REG(EXPECTED_RESOLUTION), CSI_FIELD(EXPECTED_RESOLUTION, EXPECTED_WIDTH, layout->width) |
                                                  CSI_FIELD(EXPECTED_RESOLUTION, EXPECTED_HEIGHT, layout->height));

    fx1_write32(CSI_REG(METADATA_RECORD_ID), 0U);
}

/*
 * MAS 9.1 step 8: STREAM_CONFIG, the AXI4-Stream path to the ISP.
 * ATTENTION: stream_format changes only while axis_enable = 0 (it is applied beat by beat), so
 * axis_enable is cleared first and set again afterwards. The stream resumes at the next Frame Start of
 * the Long exposure. The same pixel_packing drives this path and the DMA path.
 * stream_follow_dma_drop = 0: a capture dropped by the DMA alone still reaches the ISP.
 */
static void csi_write_stream_path(const neon_csi_config_t *config)
{
    uint32_t stream_config;

    fx1_write32(CSI_REG(STREAM_CONFIG), 0U);
    stream_config = CSI_FIELD(STREAM_CONFIG, STREAM_FORMAT,
                              (config->pixel_packing == NEON_CSI_PACKING_UNPACKED16) ? 1U : 0U);
    fx1_write32(CSI_REG(STREAM_CONFIG), stream_config);
    fx1_write32(CSI_REG(STREAM_CONFIG), stream_config | CSI_BIT(STREAM_CONFIG, AXIS_ENABLE));
}

/*
 * MAS 9.1 step 9, first half: the DMA path, with DMA_CONTROL.dma_enable = 0 (left by csi_quiesce()).
 * Every value here is sampled when a buffer is claimed, so nothing takes effect before start().
 * ATTENTION:
 * - DMA_BUFFER_SIZE and META_AREA_SIZE are per channel; one buffer is 2 x (both), VC1 always reserved.
 * - LINE_PITCH holds bytes_per_line and line_padding = stride - bytes_per_line, not the stride.
 * - DMA_ATTR is written its reset value (burst 256 beats, QoS 0) so a warm restart is deterministic.
 * - err_frame_policy: configure() passes 1 (reset value, a broken buffer is released as SW_ERROR);
 *   neon_csi_fx1_set_dma_config() passes its own between configure() and start().
 */
static void csi_write_dma_path(const neon_csi_config_t *config, const neon_csi_layout_t *layout,
                               neon_csi_fx1_error_policy_t policy)
{
    uint32_t region;
    uint32_t meta;

    region = (config->dma.buffer_size_bytes == 0U) ? layout->frame_bytes : config->dma.buffer_size_bytes;
    meta = config->metadata.enable ? config->metadata.area_size_bytes : 0U;

    fx1_write32(CSI_REG(DMA_BUFFER0_ADDR), config->dma.base_addr[0]);
    fx1_write32(CSI_REG(DMA_BUFFER1_ADDR), config->dma.base_addr[1]);
    fx1_write32(CSI_REG(DMA_BUFFER_SIZE), region);
    fx1_write32(CSI_REG(META_AREA_SIZE), meta);
    fx1_write32(CSI_REG(LINE_PITCH),
                CSI_FIELD(LINE_PITCH, BYTES_PER_LINE, layout->bytes_per_line) |
                    CSI_FIELD(LINE_PITCH, LINE_PADDING, layout->line_pitch_bytes - layout->bytes_per_line));
    fx1_write32(CSI_REG(DMA_ATTR), NEON_FX1_MIPI_CSI2_DMA_ATTR_RESET);
    fx1_write32(CSI_REG(DMA_CONTROL),
                CSI_FIELD(DMA_CONTROL, DMA_FORMAT, (config->pixel_packing == NEON_CSI_PACKING_UNPACKED16) ? 1U : 0U) |
                    CSI_FIELD(DMA_CONTROL, ERR_FRAME_POLICY, policy));
}

/*
 * Test pattern generator registers, tpg_enable excluded (start() sets it, MAS 9.1 TPG note).
 * ATTENTION:
 * - TPG_RESOLUTION and TPG_BLANKING are latched at the start of each generated frame.
 * - tpg_width is truncated to a multiple of 4 by the hardware; width and height must be >= 8 while
 *   tpg_enable = 1 (checked by neon_csi_calculate_layout()). height is per virtual channel.
 * - tpg_pixel_format: 0 = RAW12, 1 = RAW10, the opposite order of the other format fields.
 * - VC0 is always the lead channel in TPG mode; PROTOCOL_CONFIG.vc_exposure_map must be 0.
 */
static void csi_write_tpg(neon_csi_raw_format_t raw_format, const neon_csi_fx1_tpg_config_t *tpg)
{
    fx1_write32(CSI_REG(TPG_RESOLUTION), CSI_FIELD(TPG_RESOLUTION, TPG_WIDTH, tpg->base.width) |
                                             CSI_FIELD(TPG_RESOLUTION, TPG_HEIGHT, tpg->base.height));
    fx1_write32(CSI_REG(TPG_BLANKING), CSI_FIELD(TPG_BLANKING, TPG_HBLANK, tpg->hblank_cycles) |
                                           CSI_FIELD(TPG_BLANKING, TPG_VBLANK, tpg->vblank_lines));
    fx1_write32(CSI_REG(TPG_CONTROL),
                CSI_FIELD(TPG_CONTROL, PATTERN, tpg->base.pattern) |
                    CSI_FIELD(TPG_CONTROL, TPG_DUAL_VC_EN, tpg->dual_vc ? 1U : 0U) |
                    CSI_FIELD(TPG_CONTROL, TPG_PIXEL_FORMAT, (raw_format == NEON_CSI_RAW10) ? 1U : 0U) |
                    CSI_FIELD(TPG_CONTROL, TPG_CFA_ORDER, tpg->cfa_order));
}

/* IRQ_ENABLE from config.sub_event_mask. Sub events with no IRQ source (SYSTEM_LINK_RESET, raised by the
 * driver) have no entry and enable nothing. */
static uint32_t csi_irq_enable_mask(const neon_csi_config_t *config)
{
    uint32_t mask = 0U;
    uint32_t i;

    for (i = 0U; i < CSI_IRQ_MAP_COUNT; i++) {
        if ((config->sub_event_mask[csi_irq_map[i].event] & csi_irq_map[i].sub_event) != 0U) {
            mask |= csi_irq_map[i].irq_bit;
        }
    }
    return mask;
}

/*
 * MAS 9.1 TPG note: vc_exposure_map = 0, then tpg_enable = 1, then wait for CORE_STATUS.active_src = 1
 * and src_switch_pending = 0.
 * ATTENTION:
 * - vc_exposure_map is read-modify-write: active_lanes of the same register belongs to the D-PHY driver.
 * - The switch waits until no source has a frame in flight: the sensor must not stream meanwhile.
 * - No fixed maximum latency (CORE_STATUS.active_src); NEON_TIMEOUT after CSI_TPG_SWITCH_POLLS reads.
 */
static neon_status_t csi_tpg_switch_on(void)
{
    uint32_t core_status;
    uint32_t polls;

    fx1_write32(CSI_REG(PROTOCOL_CONFIG),
                fx1_read32(CSI_REG(PROTOCOL_CONFIG)) & NEON_FX1_MIPI_CSI2_PROTOCOL_CONFIG_RW_MASK &
                    ~CSI_BIT(PROTOCOL_CONFIG, VC_EXPOSURE_MAP));
    fx1_write32(CSI_REG(TPG_CONTROL), (fx1_read32(CSI_REG(TPG_CONTROL)) & NEON_FX1_MIPI_CSI2_TPG_CONTROL_RW_MASK) |
                                          CSI_BIT(TPG_CONTROL, TPG_ENABLE));
    for (polls = 0U; polls < CSI_TPG_SWITCH_POLLS; polls++) {
        core_status = fx1_read32(CSI_REG(CORE_STATUS));
        if (((core_status & CSI_BIT(CORE_STATUS, ACTIVE_SRC)) != 0U) &&
            ((core_status & CSI_BIT(CORE_STATUS, SRC_SWITCH_PENDING)) == 0U)) {
            return NEON_OK;
        }
    }
    return NEON_TIMEOUT;
}

/* Wait for CORE_STATUS.active_src = 0, the sensor path. Returns at once when the TPG was never used.
 * NEON_TIMEOUT after CSI_TPG_SWITCH_POLLS reads: a TPG frame is still in flight. */
static neon_status_t csi_wait_sensor_source(void)
{
    uint32_t polls;

    for (polls = 0U; polls < CSI_TPG_SWITCH_POLLS; polls++) {
        if ((fx1_read32(CSI_REG(CORE_STATUS)) & CSI_BIT(CORE_STATUS, ACTIVE_SRC)) == 0U) {
            return NEON_OK;
        }
    }
    return NEON_TIMEOUT;
}

/* BUFFER_OWNERSHIP bufN_state -> generic state. 00/01/10 match the enum values; 11 is reserved and is
 * reported as hardware owned, the state in which software does not touch the buffer. */
static neon_csi_buffer_state_t csi_buffer_state(uint32_t field)
{
    switch (field) {
    case 1U:
        return NEON_CSI_BUFFER_SW_VALID;
    case 2U:
        return NEON_CSI_BUFFER_SW_ERROR;
    default:
        return NEON_CSI_BUFFER_HW_OWNED;
    }
}

/*
 * Link check, done by every event call and by acquire. neon_mipi_recover() changes the epochs:
 * - phy_epoch: the D-PHY was reset under this receiver -> NEON_CSI_SUB_EVENT_SYSTEM_LINK_RESET.
 * - config_epoch: CORE_CONFIG.full_reset returned every register to reset -> the configuration is lost,
 *   the receiver goes back to IDLE and must be configured again. Buffers and the link lock are dropped.
 */
static void csi_check_link(neon_csi_handle_t *handle)
{
    if ((handle->state == NEON_CSI_STATE_RESET) || (handle->mipi == NULL)) {
        return;
    }
    if (handle->mipi->phy_epoch != handle->phy_epoch) {
        handle->phy_epoch = handle->mipi->phy_epoch;
        handle->software_sub_events |= NEON_CSI_SUB_EVENT_SYSTEM_LINK_RESET;
    }
    if (handle->mipi->config_epoch != handle->config_epoch) {
        handle->config_epoch = handle->mipi->config_epoch;
        handle->software_sub_events |= NEON_CSI_SUB_EVENT_SYSTEM_LINK_RESET;
        if ((handle->state == NEON_CSI_STATE_CONFIGURED) || (handle->state == NEON_CSI_STATE_RUNNING)) {
            if (handle->link_locked) {
                (void)neon_mipi_unlock(handle->mipi);
                handle->link_locked = false;
            }
            handle->acquired_mask = 0U;
            handle->state = NEON_CSI_STATE_IDLE;
        }
    }
}

/* Sub events of one event pending in irq_status, plus the driver's own for SYSTEM. irq_bits receives the
 * IRQ_STATUS bits behind them, for the RW1C clear. Masked sub events are included. */
static uint32_t csi_pending_sub_events(const neon_csi_handle_t *handle, uint32_t irq_status, neon_csi_event_t event,
                                       uint32_t *irq_bits)
{
    uint32_t sub_events = 0U;
    uint32_t i;

    *irq_bits = 0U;
    for (i = 0U; i < CSI_IRQ_MAP_COUNT; i++) {
        if ((csi_irq_map[i].event == event) && ((irq_status & csi_irq_map[i].irq_bit) != 0U)) {
            sub_events |= csi_irq_map[i].sub_event;
            *irq_bits |= csi_irq_map[i].irq_bit;
        }
    }
    if (event == NEON_CSI_EVENT_SYSTEM) {
        sub_events |= handle->software_sub_events;
    }
    return sub_events;
}

/* frame_id a comes before b in the hardware sequence 1, 2, ... 0x3FFF, 1 (0 skipped): the distance from a
 * forward to b is less than half the period. */
static bool csi_frame_id_before(uint32_t a, uint32_t b)
{
    uint32_t distance = ((b + CSI_FRAME_ID_PERIOD) - a) % CSI_FRAME_ID_PERIOD;

    return (distance != 0U) && (distance < (CSI_FRAME_ID_PERIOD / 2U));
}

/* One 32-bit word of a metadata record in DDR. */
static uint32_t csi_record_word(uint32_t meta_addr, uint32_t word)
{
    return *(const volatile uint32_t *)(uintptr_t)(meta_addr + (word * 4U));
}

/* METADATA_DT_CONFIG value. fnum_check_en applies to the sensor path only; emb_extract_en only while
 * metadata_capture_en = 1. */
static uint32_t csi_metadata_dt_config(uint8_t data_type, bool capture, bool fnum_check, bool extract)
{
    return CSI_FIELD(METADATA_DT_CONFIG, METADATA_DT, data_type) |
           (capture ? CSI_BIT(METADATA_DT_CONFIG, METADATA_CAPTURE_EN) : 0U) |
           (fnum_check ? CSI_BIT(METADATA_DT_CONFIG, FNUM_CHECK_EN) : 0U) |
           (extract ? CSI_BIT(METADATA_DT_CONFIG, EMB_EXTRACT_EN) : 0U);
}
