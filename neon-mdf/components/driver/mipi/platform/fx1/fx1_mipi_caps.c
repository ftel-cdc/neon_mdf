/**
 * @file fx1_mipi_caps.c
 * @brief What the FX1 D-PHY receiver provides (MAS V1.1). Facts of the IP, shared by every FX1 backend.
 */

/*==================================================================================================
*                                          INCLUDE FILES                                          
==================================================================================================*/
#include "mipi/neon_mipi.h"
#include "mipi/platform/common/neon_mipi_hw.h"

/*==================================================================================================
*                                        DEFINES AND MACROS                                       
==================================================================================================*/
#define FX1_MIPI_MAX_LANES 4U
/* The core is timed to 1792 Mb/s; 1500 Mb/s is the rate it is specified for. */
#define FX1_MIPI_MAX_LANE_RATE_BPS 1500000000UL
/* Lane Management maps byte k to lane k mod 4, so only lane counts that divide four exist: 1, 2 and 4. */
#define FX1_MIPI_LANE_COUNT_MASK ((1U << 1) | (1U << 2) | (1U << 4))

_Static_assert(FX1_MIPI_MAX_LANES <= NEON_MIPI_MAX_LANES, "raise NEON_MIPI_MAX_LANES");

/*==================================================================================================
*                                         GLOBAL FUNCTIONS                                        
==================================================================================================*/
neon_status_t neon_mipi_hw_get_caps(neon_mipi_caps_t *caps)
{
    caps->max_lanes = FX1_MIPI_MAX_LANES;
    caps->lane_count_mask = (uint8_t)FX1_MIPI_LANE_COUNT_MASK;
    caps->max_lane_rate_bps = FX1_MIPI_MAX_LANE_RATE_BPS;
    caps->rx_supported = true;
    caps->tx_supported = false;
    caps->non_continuous_clock = false;
    return NEON_OK;
}
