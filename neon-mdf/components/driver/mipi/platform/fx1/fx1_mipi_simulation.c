/**
 * @file fx1_mipi_simulation.c
 * @brief In-RAM backend of the MIPI D-PHY receive link.
 */

/*==================================================================================================
*                                          INCLUDE FILES                                          
==================================================================================================*/
#include "mipi/platform/fx1/fx1_mipi_simulation.h"
#include "mipi/platform/common/neon_mipi_hw.h"
#include <string.h>

/*==================================================================================================
*                                  STRUCTURES AND OTHER TYPEDEFS                                  
==================================================================================================*/
typedef struct {
    bool enabled;
    bool hs_active;
    uint8_t enabled_lanes;
    uint8_t active_lanes;
    uint8_t stuck_lane_mask;
    uint8_t sot_error_mask;
    uint8_t sot_sync_error_mask;
    uint16_t lane_abort_count;
    uint32_t init_polls;
    uint32_t init_polls_left;
    fx1_mipi_sim_counters_t counters;
} mipi_sim_t;

_Static_assert(sizeof(mipi_sim_t) <= sizeof(((neon_mipi_handle_t *)0)->backend_words),
               "mipi_sim_t does not fit in the handle");

/*==================================================================================================
*                                         LOCAL FUNCTIONS                                         
==================================================================================================*/
static mipi_sim_t *mipi_sim(neon_mipi_handle_t *handle)
{
    return (mipi_sim_t *)(void *)handle->backend_words;
}

static const mipi_sim_t *mipi_sim_const(const neon_mipi_handle_t *handle)
{
    return (const mipi_sim_t *)(const void *)handle->backend_words;
}

/*==================================================================================================
*                                         GLOBAL FUNCTIONS                                        
==================================================================================================*/
neon_status_t neon_mipi_hw_init(neon_mipi_handle_t *handle)
{
    (void)memset(handle->backend_words, 0, sizeof(handle->backend_words));
    return NEON_OK;
}

neon_status_t neon_mipi_hw_phy_reset(neon_mipi_handle_t *handle)
{
    mipi_sim_t *sim = mipi_sim(handle);

    sim->counters.phy_resets++;
    sim->hs_active = false;
    sim->sot_error_mask = 0U;
    sim->sot_sync_error_mask = 0U;
    sim->init_polls_left = sim->init_polls;
    return NEON_OK;
}

neon_status_t neon_mipi_hw_full_reset(neon_mipi_handle_t *handle)
{
    mipi_sim_t *sim = mipi_sim(handle);
    mipi_sim_t fresh;

    (void)memset(&fresh, 0, sizeof(fresh));
    fresh.init_polls = sim->init_polls;
    fresh.stuck_lane_mask = sim->stuck_lane_mask;
    fresh.init_polls_left = sim->init_polls;
    fresh.counters = sim->counters;
    fresh.counters.full_resets++;
    *sim = fresh;
    return NEON_OK;
}

neon_status_t neon_mipi_hw_phy_enable(neon_mipi_handle_t *handle, uint8_t lane_count)
{
    mipi_sim_t *sim = mipi_sim(handle);

    sim->enabled = true;
    sim->enabled_lanes = lane_count;
    sim->init_polls_left = sim->init_polls;
    return NEON_OK;
}

neon_status_t neon_mipi_hw_phy_disable(neon_mipi_handle_t *handle)
{
    mipi_sim_t *sim = mipi_sim(handle);

    sim->enabled = false;
    sim->enabled_lanes = 0U;
    sim->active_lanes = 0U;
    sim->hs_active = false;
    return NEON_OK;
}

neon_status_t neon_mipi_hw_read_link(neon_mipi_handle_t *handle, neon_mipi_hw_link_t *link)
{
    mipi_sim_t *sim = mipi_sim(handle);
    uint8_t lane_mask;

    (void)memset(link, 0, sizeof(*link));
    link->sot_error_mask = sim->sot_error_mask;
    link->sot_sync_error_mask = sim->sot_sync_error_mask;
    link->lane_abort_count = sim->lane_abort_count;
    if (!sim->enabled) {
        return NEON_OK;
    }
    if (sim->init_polls_left != 0U) {
        sim->init_polls_left--;
        return NEON_OK;
    }
    link->init_done = true;
    if (sim->hs_active) {
        link->clock_hs_active = true;
        return NEON_OK;
    }
    lane_mask = (uint8_t)((1U << sim->enabled_lanes) - 1U);
    link->clock_stop_state = true;
    link->lane_stop_mask = (uint8_t)(lane_mask & (uint8_t)~sim->stuck_lane_mask);
    return NEON_OK;
}

neon_status_t neon_mipi_hw_set_active_lanes(neon_mipi_handle_t *handle, uint8_t lane_count)
{
    mipi_sim_t *sim = mipi_sim(handle);

    /* The value is adopted only while Lane Management is idle, which it is not during a burst. */
    if (sim->hs_active) {
        return NEON_BUSY;
    }
    sim->active_lanes = lane_count;
    return NEON_OK;
}

void fx1_mipi_sim_set_init_polls(neon_mipi_handle_t *handle, uint32_t polls)
{
    mipi_sim(handle)->init_polls = polls;
    mipi_sim(handle)->init_polls_left = polls;
}

void fx1_mipi_sim_set_stuck_lanes(neon_mipi_handle_t *handle, uint8_t lane_mask)
{
    mipi_sim(handle)->stuck_lane_mask = lane_mask;
}

void fx1_mipi_sim_set_hs_active(neon_mipi_handle_t *handle, bool active)
{
    mipi_sim(handle)->hs_active = active;
}

void fx1_mipi_sim_inject_lane_errors(neon_mipi_handle_t *handle, uint8_t sot_mask, uint8_t sot_sync_mask, uint16_t aborts)
{
    mipi_sim_t *sim = mipi_sim(handle);

    sim->sot_error_mask |= sot_mask;
    sim->sot_sync_error_mask |= sot_sync_mask;
    sim->lane_abort_count = (uint16_t)(sim->lane_abort_count + aborts);
}

void fx1_mipi_sim_get_counters(const neon_mipi_handle_t *handle, fx1_mipi_sim_counters_t *counters)
{
    *counters = mipi_sim_const(handle)->counters;
}

uint8_t fx1_mipi_sim_get_active_lanes(const neon_mipi_handle_t *handle)
{
    return mipi_sim_const(handle)->active_lanes;
}
