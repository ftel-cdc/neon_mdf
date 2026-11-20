/**
 * @file fx1_isp_profile.c
 * @brief Which FX1 ISP registers a tuning profile may write. A fact of the register map, shared by every
 * FX1 backend.
 * @details
 * The register list, offsets and access types come from the generated register header (FX1.yaml), so a
 * profile is checked against the register map and not against a list copied by hand. What is decided here
 * is only the policy:
 *  - an offset that is not a register, a read-only register, or a register with write-one-to-clear bits is
 *    refused: a tuning set has no business clearing status, and such an entry means the file is wrong;
 *  - registers that the HAL programs from neon_isp_config_t or at run time are skipped, so that the HAL's
 *    value wins. They are the frame geometry, Bayer order and input depth, the pipeline enable, the
 *    interrupts, both DMA engines and their buffers, the output format, output enables and Resizer (the
 *    output size the HAL reports), the extra output channels and the statistics context ids and AWB bank
 *    lease, which belong to the 3A loop.
 * Everything else (BLC, LSC, BPC, WB, DG, demosaic, CCM, gamma, CSC, GTM, NR, TNR, EE, CNF, the statistics
 * set-up and the TUNE interface) is written.
 */

/*==================================================================================================
*                                          INCLUDE FILES                                          
==================================================================================================*/
#include "isp/neon_isp.h"
#include "isp/platform/common/neon_isp_hw.h"
#include "fx1_registers.h"
#include <string.h>

/*==================================================================================================
*                                  STRUCTURES AND OTHER TYPEDEFS                                  
==================================================================================================*/
typedef struct {
    uint32_t offset;
    uint32_t access;
    uint32_t w1c_mask;                 /* Bits of the register that a write of one clears. */
    const char *name;
} fx1_isp_register_t;

/*==================================================================================================
*                                         LOCAL CONSTANTS                                         
==================================================================================================*/
#define FX1_ISP_REGISTER(name, offset, access) {offset, access, NEON_FX1_ISP_##name##_W1C_MASK, #name},

/* In increasing offset order: the generator requires it of the register map. */
static const fx1_isp_register_t FX1_ISP_REGISTERS[] = {NEON_FX1_ISP_REGISTER_LIST(FX1_ISP_REGISTER)};

_Static_assert(sizeof(FX1_ISP_REGISTERS) / sizeof(FX1_ISP_REGISTERS[0]) == NEON_FX1_ISP_REGISTER_COUNT,
               "register list and count disagree");

/* Registers the HAL owns, by exact name. */
static const char *const FX1_ISP_OWNED[] = {
    "COMMON_CTRL",        "COMMON_IRQ_EN",  "COMMON_FRAME_WIDTH", "COMMON_FRAME_HEIGHT",
    "COMMON_BAYER",       "COMMON_INPUT_FORMAT",
    "AEC_CONTEXT_ID",     "AWB_CONTEXT_ID", "AF_CONTEXT_ID",      "AWB_RESULT_CTRL",
};

/* Register groups the HAL owns entirely, by name prefix. */
static const char *const FX1_ISP_OWNED_GROUPS[] = {
    "DMA_", "IDMA_", "ODMA_", "FRAME_", "OFMT_", "OUTPUT_", "RESIZER_", "CH0_", "CH1_", "CH2_",
};

/*==================================================================================================
*                                         LOCAL FUNCTIONS                                         
==================================================================================================*/
static const fx1_isp_register_t *fx1_isp_find(uint32_t offset)
{
    size_t low = 0U;
    size_t high = sizeof(FX1_ISP_REGISTERS) / sizeof(FX1_ISP_REGISTERS[0]);

    while (low < high) {
        size_t middle = low + (high - low) / 2U;

        if (FX1_ISP_REGISTERS[middle].offset == offset) {
            return &FX1_ISP_REGISTERS[middle];
        }
        if (FX1_ISP_REGISTERS[middle].offset < offset) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    return NULL;
}

static bool fx1_isp_owned(const char *name)
{
    size_t i;

    for (i = 0U; i < sizeof(FX1_ISP_OWNED) / sizeof(FX1_ISP_OWNED[0]); i++) {
        if (strcmp(name, FX1_ISP_OWNED[i]) == 0) {
            return true;
        }
    }
    for (i = 0U; i < sizeof(FX1_ISP_OWNED_GROUPS) / sizeof(FX1_ISP_OWNED_GROUPS[0]); i++) {
        if (strncmp(name, FX1_ISP_OWNED_GROUPS[i], strlen(FX1_ISP_OWNED_GROUPS[i])) == 0) {
            return true;
        }
    }
    return false;
}

/*==================================================================================================
*                                        GLOBAL FUNCTIONS                                         
==================================================================================================*/
neon_isp_hw_profile_action_t neon_isp_hw_profile_classify(uint32_t offset)
{
    const fx1_isp_register_t *reg = fx1_isp_find(offset);

    if (reg == NULL || reg->access == NEON_FX1_ACCESS_RO || reg->w1c_mask != 0U) {
        return NEON_ISP_HW_PROFILE_REJECT;
    }
    return fx1_isp_owned(reg->name) ? NEON_ISP_HW_PROFILE_SKIP : NEON_ISP_HW_PROFILE_WRITE;
}
