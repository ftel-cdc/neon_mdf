/* Shared register metadata, access kinds and MMIO/field helpers. */
#ifndef FTEL_FX1_ISP_REGMAP_V107_BASE_H
#define FTEL_FX1_ISP_REGMAP_V107_BASE_H

#include <stdint.h>
#include <stddef.h>
#include "fx1_isp/fx1_isp_csr.h"
#define FX1_ISP_REG_COUNT 621u
#define FX1_ISP_FIELD_COUNT 867u
#define FX1_ISP_MAX_REGISTER_OFFSET 0x8C90u
#define FX1_ISP_EXPECTED_VER_ID FX1_ISP_COMMON_VER_ID_RESET
#define FX1_ISP_EXPECTED_VER_DATE FX1_ISP_COMMON_VER_DATE_RESET
#define FX1_ISP_EXPECTED_TUNE_REV FX1_ISP_TUNE_REV_RESET
#define FX1_ISP_ACCESS_RO 0u
#define FX1_ISP_ACCESS_RW 1u
#define FX1_ISP_ACCESS_W1C 2u
#define FX1_ISP_ACCESS_W1S 3u
#define FX1_ISP_ACCESS_W1P 4u
#define FX1_ISP_ACCESS_W1SC 5u
#define FX1_ISP_ACCESS_MIXED 6u
static inline uint32_t fx1_isp_mmio_read32(uintptr_t base, uint32_t offset)
{ return *(volatile const uint32_t *)(base + offset); }
static inline void fx1_isp_mmio_write32(uintptr_t base, uint32_t offset, uint32_t value)
{ *(volatile uint32_t *)(base + offset) = value; }
static inline uint32_t fx1_isp_field_get(uint32_t word, uint32_t mask, uint32_t shift)
{ return (word & mask) >> shift; }
static inline uint32_t fx1_isp_field_prep(uint32_t value, uint32_t mask, uint32_t shift)
{ return (value << shift) & mask; }

#endif
