/**
 * @file neon_mipi.h
 * @brief Public declarations for the MIPI D-PHY receive link.
 * @details
 * D-PHY receive link. This module owns D-PHY power, lane enables, Stop State detection, the active lane
 * count and link recovery. It never interprets CSI-2 packets: neon_csi.h is its client and takes a lock on
 * the link while it receives.
 *
 * This header holds the contract only. What a chip supports (lane counts, line rate, direction, clock
 * lane mode) is reported by neon_mipi_get_caps() and enforced from there, so that another chip changes its
 * backend and not this file. The first chip is the FX1 FTEL CSI-2 controller, D-PHY side (MAS V1.1
 * chapters 6.3, 6.4 and 9.1), whose MIPI and CSI register banks are one 4 KB CSR block: both modules share
 * one backend register layer and only the API is split.
 * @note Blocking calls are intended for task context. timeout_polls values are polling budgets, not
 * milliseconds.
 */

#ifndef __NEON_MIPI_H__
#define __NEON_MIPI_H__

/*==================================================================================================
*                                          INCLUDE FILES                                          
==================================================================================================*/
#include <stdbool.h>
#include <stdint.h>
#include "neon_types.h"
#ifdef __cplusplus
extern "C" {
#endif

/*==================================================================================================
*                                        DEFINES AND MACROS                                       
==================================================================================================*/
/* Capacity of the structures: the most data lanes any supported chip has. The real limits are in neon_mipi_caps_t. */
#define NEON_MIPI_MAX_LANES 4U
/* Words of backend-private storage inside the handle. */
#define NEON_MIPI_BACKEND_WORDS 4U

/*==================================================================================================
*                                  STRUCTURES AND OTHER TYPEDEFS                                  
==================================================================================================*/
typedef enum {
    NEON_MIPI_DIRECTION_RX = 0,
    NEON_MIPI_DIRECTION_TX = 1
} neon_mipi_direction_t;

typedef enum {
    NEON_MIPI_STATE_RELEASED = 0, /* PHY disabled, no lanes owned. */
    NEON_MIPI_STATE_ENABLED = 1,  /* PHY and lanes enabled; Stop State not yet confirmed. */
    NEON_MIPI_STATE_READY = 2     /* Clock and active lanes in Stop State, active_lanes applied. */
} neon_mipi_state_t;

typedef enum {
    /* D-PHY macro reset only (PHY_CONFIG.dphy_rst_req). CSI register configuration is kept. */
    NEON_MIPI_RECOVER_PHY = 0,
    /* D-PHY and whole CSI datapath, including the register file (CORE_CONFIG.full_reset). Every CSI
     * receiver bound to the link must be configured again. */
    NEON_MIPI_RECOVER_FULL = 1
} neon_mipi_recover_level_t;

/** What one chip provides. The core validates from these values only. */
typedef struct {
    uint8_t max_lanes;             /* At most NEON_MIPI_MAX_LANES. */
    uint8_t lane_count_mask;       /* Bit n set: n data lanes are supported. */
    uint32_t max_lane_rate_bps;    /* Line rate per data lane. */
    bool rx_supported;
    bool tx_supported;
    bool non_continuous_clock;     /* The clock lane may return to Low-Power mode between bursts. */
} neon_mipi_caps_t;

typedef struct {
    neon_mipi_direction_t direction;
    uint8_t lane_count;       /* One of caps.lane_count_mask. */
    uint32_t lane_rate_bps;   /* Line rate per data lane. */
    bool continuous_clock;    /* False needs caps.non_continuous_clock. */
} neon_mipi_config_t;

typedef struct {
    neon_mipi_state_t state;
    uint8_t lane_count;
    uint32_t lane_rate_bps;
    uint8_t lock_count;       /* Receivers currently holding the link. */
    uint32_t phy_epoch;       /* Incremented by every recovery. */
    uint32_t config_epoch;    /* Incremented by NEON_MIPI_RECOVER_FULL: CSI configuration was lost. */
    bool clock_stop_state;
    bool clock_ulps;
    bool clock_hs_active;     /* rxbyteclkhs is running, which soft_reset needs. */
    uint8_t lane_stop_mask;   /* Bit n set: data lane n is in Stop State. */
    uint8_t sot_error_mask;
    uint8_t sot_sync_error_mask;
    uint16_t lane_abort_count;
} neon_mipi_status_t;

typedef struct neon_mipi_handle_s {
    neon_mipi_state_t state;
    uint8_t lane_count;
    uint8_t lock_count;
    uint32_t lane_rate_bps;
    uint32_t phy_epoch;
    uint32_t config_epoch;
    uint64_t backend_words[NEON_MIPI_BACKEND_WORDS]; /* Private to the selected backend. */
} neon_mipi_handle_t;

/*==================================================================================================
*                                       FUNCTION PROTOTYPES                                       
==================================================================================================*/
/**
 * @brief Initialize a link handle. Touches no hardware state beyond the backend's own bookkeeping.
 * @param[out] handle Link handle, caller allocated.
 * @return NEON_OK on success, NEON_INVALID_PARAM for a NULL handle.
 */
neon_status_t neon_mipi_init(neon_mipi_handle_t *handle);

/**
 * @brief Deinitialize a link handle.
 * @param[in] handle Link handle.
 * @return NEON_OK, or NEON_BUSY while the link is still acquired.
 */
neon_status_t neon_mipi_deinit(neon_mipi_handle_t *handle);

/**
 * @brief Read the capabilities of the D-PHY side.
 * @param[out] caps Capabilities.
 * @return NEON_OK on success, NEON_INVALID_PARAM for a NULL pointer.
 */
neon_status_t neon_mipi_get_caps(neon_mipi_caps_t *caps);

/**
 * @brief Reset and enable the D-PHY and the lanes (MAS 9.1 steps 1 to 3).
 * @param[in,out] handle Link handle in NEON_MIPI_STATE_RELEASED.
 * @param[in] config Link configuration.
 * @return NEON_OK on success, NEON_BUSY if already acquired, NEON_NOT_SUPPORTED for TX, three lanes,
 * a rate above the maximum or a non-continuous clock, NEON_INVALID_PARAM for other bad arguments,
 * or a backend status.
 * @details
 * The sensor must be powered and driving LP-11 before neon_mipi_wait_ready() can succeed.
 */
neon_status_t neon_mipi_acquire(neon_mipi_handle_t *handle, const neon_mipi_config_t *config);

/**
 * @brief Wait for Stop State on the clock lane and every active lane, then apply active_lanes
 * (MAS 9.1 steps 4 and 5).
 * @param[in,out] handle Link handle.
 * @param[in] timeout_polls Status polls to spend, at least one. The Stop State is reached only
 * after the transmitter has driven LP-11 for T_INIT, which is at least 100 us.
 * @return NEON_OK when the link is NEON_MIPI_STATE_READY, NEON_TIMEOUT if the budget ran out (the
 * state stays NEON_MIPI_STATE_ENABLED and the call may be repeated), NEON_ERROR if the link is not
 * acquired.
 */
neon_status_t neon_mipi_wait_ready(neon_mipi_handle_t *handle, uint32_t timeout_polls);

/**
 * @brief Read the link state and the PHY and lane indications.
 * @param[in] handle Link handle.
 * @param[out] status Snapshot. Indications are zero while the link is released.
 * @return NEON_OK on success, NEON_INVALID_PARAM for a NULL pointer.
 */
neon_status_t neon_mipi_get_status(neon_mipi_handle_t *handle, neon_mipi_status_t *status);

/**
 * @brief Reset the link and bring it back to NEON_MIPI_STATE_READY.
 * @param[in,out] handle Link handle, acquired.
 * @param[in] level Recovery scope.
 * @param[in] timeout_polls Polling budget of the Stop State wait.
 * @return Result of the Stop State wait, or NEON_ERROR if the link is not acquired.
 * @details
 * A recovery may run while a receiver holds the link. NEON_MIPI_RECOVER_PHY changes phy_epoch, so the
 * receiver reports a link reset. NEON_MIPI_RECOVER_FULL also changes config_epoch: the receiver
 * must be configured again.
 */
neon_status_t neon_mipi_recover(neon_mipi_handle_t *handle, neon_mipi_recover_level_t level, uint32_t timeout_polls);

/**
 * @brief Disable the lanes and the D-PHY.
 * @param[in,out] handle Link handle.
 * @return NEON_OK on success, NEON_LOCKED while a receiver holds the link, NEON_ERROR if the link is
 * not acquired.
 */
neon_status_t neon_mipi_release(neon_mipi_handle_t *handle);

/**
 * @brief Pin the link for a receiver so that it cannot be released under it.
 * @param[in,out] handle Link handle in NEON_MIPI_STATE_READY.
 * @return NEON_OK on success, NEON_ERROR if the link is not ready.
 */
neon_status_t neon_mipi_lock(neon_mipi_handle_t *handle);

/**
 * @brief Drop a pin taken with neon_mipi_lock().
 * @param[in,out] handle Link handle.
 * @return NEON_OK on success, NEON_ERROR if nothing was locked.
 */
neon_status_t neon_mipi_unlock(neon_mipi_handle_t *handle);

#ifdef __cplusplus
}
#endif

#endif /* __NEON_MIPI_H__ */
