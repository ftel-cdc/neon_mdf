/**
 * @file neon_mipi.c
 * @brief MIPI D-PHY receive link: state machine and argument rules, independent of the backend.
 */

/*==================================================================================================
*                                          INCLUDE FILES                                          
==================================================================================================*/
#include "mipi/neon_mipi.h"
#include "mipi/platform/common/neon_mipi_hw.h"
#include <stddef.h>
#include <string.h>

/*==================================================================================================
*                                         LOCAL FUNCTIONS                                         
==================================================================================================*/
static neon_status_t mipi_validate_config(const neon_mipi_config_t *config, const neon_mipi_caps_t *caps)
{
    if (config->direction != NEON_MIPI_DIRECTION_RX && config->direction != NEON_MIPI_DIRECTION_TX) {
        return NEON_INVALID_PARAM;
    }
    if (config->direction == NEON_MIPI_DIRECTION_TX) {
        return caps->tx_supported ? NEON_OK : NEON_NOT_SUPPORTED;
    }
    if (config->lane_count == 0U || config->lane_count > caps->max_lanes || config->lane_rate_bps == 0U) {
        return NEON_INVALID_PARAM;
    }
    if (((caps->lane_count_mask >> config->lane_count) & 1U) == 0U) {
        return NEON_NOT_SUPPORTED;
    }
    if (config->lane_rate_bps > caps->max_lane_rate_bps ||
        (!config->continuous_clock && !caps->non_continuous_clock)) {
        return NEON_NOT_SUPPORTED;
    }
    return NEON_OK;
}

static uint8_t mipi_lane_mask(uint8_t lane_count)
{
    return (uint8_t)((1U << lane_count) - 1U);
}

/*==================================================================================================
*                                         GLOBAL FUNCTIONS                                        
==================================================================================================*/
neon_status_t neon_mipi_init(neon_mipi_handle_t *handle)
{
    if (handle == NULL) {
        return NEON_INVALID_PARAM;
    }
    (void)memset(handle, 0, sizeof(*handle));
    handle->state = NEON_MIPI_STATE_RELEASED;
    return neon_mipi_hw_init(handle);
}

neon_status_t neon_mipi_deinit(neon_mipi_handle_t *handle)
{
    if (handle == NULL) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state != NEON_MIPI_STATE_RELEASED) {
        return NEON_BUSY;
    }
    (void)memset(handle, 0, sizeof(*handle));
    return NEON_OK;
}

neon_status_t neon_mipi_get_caps(neon_mipi_caps_t *caps)
{
    if (caps == NULL) {
        return NEON_INVALID_PARAM;
    }
    return neon_mipi_hw_get_caps(caps);
}

neon_status_t neon_mipi_acquire(neon_mipi_handle_t *handle, const neon_mipi_config_t *config)
{
    neon_mipi_caps_t caps;
    neon_status_t status;

    if (handle == NULL || config == NULL) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state != NEON_MIPI_STATE_RELEASED) {
        return NEON_BUSY;
    }
    status = neon_mipi_hw_get_caps(&caps);
    if (status == NEON_OK) {
        status = mipi_validate_config(config, &caps);
    }
    if (status == NEON_OK) {
        status = neon_mipi_hw_phy_reset(handle);
    }
    if (status == NEON_OK) {
        status = neon_mipi_hw_phy_enable(handle, config->lane_count);
        if (status != NEON_OK) {
            (void)neon_mipi_hw_phy_disable(handle);
        }
    }
    if (status != NEON_OK) {
        return status;
    }
    handle->lane_count = config->lane_count;
    handle->lane_rate_bps = config->lane_rate_bps;
    handle->state = NEON_MIPI_STATE_ENABLED;
    return NEON_OK;
}

neon_status_t neon_mipi_wait_ready(neon_mipi_handle_t *handle, uint32_t timeout_polls)
{
    neon_mipi_hw_link_t link;
    neon_status_t status;
    uint8_t lane_mask;
    uint32_t polls;

    if (handle == NULL || timeout_polls == 0U) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state == NEON_MIPI_STATE_READY) {
        return NEON_OK;
    }
    if (handle->state != NEON_MIPI_STATE_ENABLED) {
        return NEON_ERROR;
    }
    lane_mask = mipi_lane_mask(handle->lane_count);
    for (polls = 0U; polls < timeout_polls; polls++) {
        status = neon_mipi_hw_read_link(handle, &link);
        if (status != NEON_OK) {
            return status;
        }
        if (link.clock_stop_state && (link.lane_stop_mask & lane_mask) == lane_mask) {
            status = neon_mipi_hw_set_active_lanes(handle, handle->lane_count);
            if (status == NEON_OK) {
                handle->state = NEON_MIPI_STATE_READY;
            }
            return status;
        }
    }
    return NEON_TIMEOUT;
}

neon_status_t neon_mipi_get_status(neon_mipi_handle_t *handle, neon_mipi_status_t *status)
{
    neon_mipi_hw_link_t link;
    neon_status_t result;

    if (handle == NULL || status == NULL) {
        return NEON_INVALID_PARAM;
    }
    (void)memset(status, 0, sizeof(*status));
    status->state = handle->state;
    status->lane_count = handle->lane_count;
    status->lane_rate_bps = handle->lane_rate_bps;
    status->lock_count = handle->lock_count;
    status->phy_epoch = handle->phy_epoch;
    status->config_epoch = handle->config_epoch;
    if (handle->state == NEON_MIPI_STATE_RELEASED) {
        return NEON_OK;
    }
    result = neon_mipi_hw_read_link(handle, &link);
    if (result != NEON_OK) {
        return result;
    }
    status->clock_stop_state = link.clock_stop_state;
    status->clock_ulps = link.clock_ulps;
    status->clock_hs_active = link.clock_hs_active;
    status->lane_stop_mask = link.lane_stop_mask;
    status->sot_error_mask = link.sot_error_mask;
    status->sot_sync_error_mask = link.sot_sync_error_mask;
    status->lane_abort_count = link.lane_abort_count;
    return NEON_OK;
}

neon_status_t neon_mipi_recover(neon_mipi_handle_t *handle, neon_mipi_recover_level_t level, uint32_t timeout_polls)
{
    neon_status_t status;

    if (handle == NULL || timeout_polls == 0U ||
        (level != NEON_MIPI_RECOVER_PHY && level != NEON_MIPI_RECOVER_FULL)) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state == NEON_MIPI_STATE_RELEASED) {
        return NEON_ERROR;
    }
    status = (level == NEON_MIPI_RECOVER_FULL) ? neon_mipi_hw_full_reset(handle) : neon_mipi_hw_phy_reset(handle);
    if (status != NEON_OK) {
        return status;
    }
    handle->phy_epoch++;
    if (level == NEON_MIPI_RECOVER_FULL) {
        handle->config_epoch++;
    }
    handle->state = NEON_MIPI_STATE_ENABLED;
    status = neon_mipi_hw_phy_enable(handle, handle->lane_count);
    if (status != NEON_OK) {
        return status;
    }
    return neon_mipi_wait_ready(handle, timeout_polls);
}

neon_status_t neon_mipi_release(neon_mipi_handle_t *handle)
{
    if (handle == NULL) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state == NEON_MIPI_STATE_RELEASED) {
        return NEON_ERROR;
    }
    if (handle->lock_count != 0U) {
        return NEON_LOCKED;
    }
    (void)neon_mipi_hw_phy_disable(handle);
    handle->state = NEON_MIPI_STATE_RELEASED;
    handle->lane_count = 0U;
    handle->lane_rate_bps = 0U;
    return NEON_OK;
}

neon_status_t neon_mipi_lock(neon_mipi_handle_t *handle)
{
    if (handle == NULL) {
        return NEON_INVALID_PARAM;
    }
    if (handle->state != NEON_MIPI_STATE_READY || handle->lock_count == UINT8_MAX) {
        return NEON_ERROR;
    }
    handle->lock_count++;
    return NEON_OK;
}

neon_status_t neon_mipi_unlock(neon_mipi_handle_t *handle)
{
    if (handle == NULL) {
        return NEON_INVALID_PARAM;
    }
    if (handle->lock_count == 0U) {
        return NEON_ERROR;
    }
    handle->lock_count--;
    return NEON_OK;
}
