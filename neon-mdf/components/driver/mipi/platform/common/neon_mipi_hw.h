/**
 * @file neon_mipi_hw.h
 * @brief @internal Register-level contract of the MIPI backends.
 * @details
 * neon_mipi.c holds every rule of the module and is the same for every chip. A chip supplies these
 * functions in exactly one backend: the in-RAM simulation (fx1_mipi_simulation.c) or its register backend
 * (the FX1 one needs the final register workbook and does not exist yet), plus its limits through
 * neon_mipi_hw_get_caps(). Applications must not include this file.
 */

#ifndef __NEON_MIPI_HW_H__
#define __NEON_MIPI_HW_H__

/*==================================================================================================
*                                          INCLUDE FILES                                          
==================================================================================================*/
#include "mipi/neon_mipi.h"
#ifdef __cplusplus
extern "C" {
#endif

/*==================================================================================================
*                                  STRUCTURES AND OTHER TYPEDEFS                                  
==================================================================================================*/
typedef struct {
    bool clock_stop_state;     /* PHY_STATUS.cl_stopstate */
    bool clock_ulps;           /* PHY_STATUS.cl_ulps */
    bool clock_hs_active;      /* PHY_STATUS.cl_rxclkactivehs */
    bool init_done;            /* PHY_STATUS.init_done */
    uint8_t lane_stop_mask;    /* LANE_STATUS.lane_stopstate */
    uint8_t sot_error_mask;
    uint8_t sot_sync_error_mask;
    uint16_t lane_abort_count; /* LANE_ABORT_COUNT */
} neon_mipi_hw_link_t;

/*==================================================================================================
*                                       FUNCTION PROTOTYPES                                       
==================================================================================================*/
neon_status_t neon_mipi_hw_init(neon_mipi_handle_t *handle);
/** Limits of the chip; FX1 defines it in fx1_mipi_caps.c, shared by its backends. */
neon_status_t neon_mipi_hw_get_caps(neon_mipi_caps_t *caps);
/** PHY_CONFIG.dphy_rst_req, then poll CORE_STATUS.reset_in_progress until it reads 0. */
neon_status_t neon_mipi_hw_phy_reset(neon_mipi_handle_t *handle);
/** CORE_CONFIG.full_reset, then poll CORE_STATUS.reset_in_progress until it reads 0. */
neon_status_t neon_mipi_hw_full_reset(neon_mipi_handle_t *handle);
/** PHY_CONFIG phy_enable, cl_enable and dl_enable for lanes 0 .. lane_count - 1. */
neon_status_t neon_mipi_hw_phy_enable(neon_mipi_handle_t *handle, uint8_t lane_count);
neon_status_t neon_mipi_hw_phy_disable(neon_mipi_handle_t *handle);
/** One poll of PHY_STATUS, LANE_STATUS and LANE_ABORT_COUNT. */
neon_status_t neon_mipi_hw_read_link(neon_mipi_handle_t *handle, neon_mipi_hw_link_t *link);
/** Write PROTOCOL_CONFIG.active_lanes and verify it through active_lanes_applied. */
neon_status_t neon_mipi_hw_set_active_lanes(neon_mipi_handle_t *handle, uint8_t lane_count);

#ifdef __cplusplus
}
#endif

#endif /* __NEON_MIPI_HW_H__ */
