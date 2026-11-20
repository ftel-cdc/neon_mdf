/**
 * @file fx1_mipi_simulation.h
 * @brief Test hooks of the in-RAM MIPI D-PHY backend.
 * @details
 * Models the Stop State and High-Speed behaviour of the link per MAS V1.1 chapter 6.3 and 9.1 so that
 * the camera runtime can be exercised without the VP or hardware. Not part of the production API.
 */

#ifndef __FX1_MIPI_SIMULATION_H__
#define __FX1_MIPI_SIMULATION_H__

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
    uint32_t phy_resets;
    uint32_t full_resets;
} fx1_mipi_sim_counters_t;

/*==================================================================================================
*                                       FUNCTION PROTOTYPES                                       
==================================================================================================*/
/** Status polls that pass before the lanes show Stop State (models T_INIT). Default 0. */
void fx1_mipi_sim_set_init_polls(neon_mipi_handle_t *handle, uint32_t polls);
/** Lanes that never reach Stop State. */
void fx1_mipi_sim_set_stuck_lanes(neon_mipi_handle_t *handle, uint8_t lane_mask);
/** The sensor starts or stops streaming: the clock lane enters or leaves High-Speed mode. */
void fx1_mipi_sim_set_hs_active(neon_mipi_handle_t *handle, bool active);
void fx1_mipi_sim_inject_lane_errors(neon_mipi_handle_t *handle, uint8_t sot_mask, uint8_t sot_sync_mask, uint16_t aborts);
void fx1_mipi_sim_get_counters(const neon_mipi_handle_t *handle, fx1_mipi_sim_counters_t *counters);
/** Lane count last applied through active_lanes. */
uint8_t fx1_mipi_sim_get_active_lanes(const neon_mipi_handle_t *handle);

#ifdef __cplusplus
}
#endif

#endif /* __FX1_MIPI_SIMULATION_H__ */
