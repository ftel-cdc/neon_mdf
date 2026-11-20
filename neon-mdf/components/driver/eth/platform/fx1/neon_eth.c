/**
 * @file neon_eth.c
 * @brief Neon Ethernet API for the FTEL FX1 MAC and its descriptor DMA.
 * @details Reference core is isolated in reference/; no RTOS or VP dependency.
 */

/*==================================================================================================
*                                          INCLUDE FILES
==================================================================================================*/
#include "eth/neon_eth.h"
#include "reference/fx1_eth.h"
#include "reference/eth_regs.h"
#include "fx1_fw.h"
#include "sdk_irq.h"
#include <stddef.h>

/*==================================================================================================
*                                  STRUCTURES AND OTHER TYPEDEFS
==================================================================================================*/
struct neon_eth_handle_s {
    fx1_eth mac;
    uint32_t acquired;
    uint32_t interrupts;
};

/*==================================================================================================
*                                        LOCAL VARIABLES
==================================================================================================*/
static neon_eth_handle_t eth_handle;

/*==================================================================================================
*                                   PRIVATE FUNCTION PROTOTYPES
==================================================================================================*/
static int eth_valid(const neon_eth_handle_t *handle);
static neon_status_t eth_status(int result);
static uint32_t eth_mask_irq(void);
static void eth_restore_irq(uint32_t enabled);

/*==================================================================================================
*                                         GLOBAL FUNCTIONS
==================================================================================================*/
/** @copydoc neon_eth_init */
neon_status_t neon_eth_init(neon_eth_handle_t **handle, neon_eth_t instance) {
    if (handle == NULL || instance != NEON_ETH0 || fx1_hartid() != 0U) {
        return NEON_INVALID_PARAM;
    }
    if (eth_handle.acquired != 0U) {
        return NEON_BUSY;
    }
    fx1_eth_setup(&eth_handle.mac);
    eth_handle.acquired = 1U;
    eth_handle.interrupts = 0U;
    *handle = &eth_handle;
    return NEON_OK;
}

/** @copydoc neon_eth_set_config */
neon_status_t neon_eth_set_config(neon_eth_handle_t *handle, const neon_eth_config_t *config) {
    if (!eth_valid(handle) || config == NULL || (config->speed_mbps != 10U && config->speed_mbps != 100U) ||
        config->phy_loopback > 1U || (config->mac_address[0] & 1U) != 0U) {
        return NEON_INVALID_PARAM;
    }
    uint32_t nonzero = 0U;
    for (uint32_t i = 0U; i < 6U; ++i) {
        nonzero |= config->mac_address[i];
    }
    if (nonzero == 0U) {
        return NEON_INVALID_PARAM;
    }
    if (handle->mac.state != FX1_ETH_UNINIT) {
        return NEON_BUSY;
    }
    for (uint32_t i = 0U; i < 6U; ++i) {
        handle->mac.cfg.mac[i] = config->mac_address[i];
    }
    handle->mac.speed100 = config->speed_mbps == 100U;
    handle->mac.cfg.phy_loopback = (int)config->phy_loopback;
    return NEON_OK;
}

/** @copydoc neon_eth_register_callback */
neon_status_t neon_eth_register_callback(neon_eth_handle_t *handle, neon_eth_rx_callback_t callback, void *context) {
    if (!eth_valid(handle)) {
        return NEON_INVALID_PARAM;
    }
    handle->mac.rx_cb = callback;
    handle->mac.rx_ctx = context;
    return NEON_OK;
}

/** @copydoc neon_eth_probe */
neon_status_t neon_eth_probe(neon_eth_handle_t *handle, neon_eth_info_t *info) {
    if (!eth_valid(handle) || info == NULL) {
        return NEON_INVALID_PARAM;
    }
    if (handle->mac.state == FX1_ETH_FAULTED) {
        return NEON_BUSY;
    }
    fx1_eth_probe_info probe;
    int result = fx1_eth_probe(&handle->mac, &probe);
    if (result == ETH_OK) {
        info->mac_id = probe.id;
        info->build_config = probe.buildcfg;
        info->phy_id1 = probe.phy_id1;
        info->phy_id2 = probe.phy_id2;
        info->link_up = (uint32_t)probe.link;
    }
    return eth_status(result);
}

/** @copydoc neon_eth_start */
neon_status_t neon_eth_start(neon_eth_handle_t *handle) {
    if (!eth_valid(handle)) {
        return NEON_INVALID_PARAM;
    }
    uint32_t enabled = eth_mask_irq();
    int result = fx1_eth_start(&handle->mac);
    /* Failed initialization may have issued DMA writes. Retain memory until
     * recovery proves quiescence; never allow setup/deinit to reuse it. */
    if (result != ETH_OK) {
        handle->mac.state = FX1_ETH_FAULTED;
    }
    eth_restore_irq(enabled);
    return eth_status(result);
}

/** @copydoc neon_eth_transmit */
neon_status_t neon_eth_transmit(neon_eth_handle_t *handle, const uint8_t *frame, uint32_t size_bytes) {
    if (!eth_valid(handle) || frame == NULL || size_bytes < 60U || size_bytes > 1514U) {
        return NEON_INVALID_PARAM;
    }
    return eth_status(fx1_eth_send(&handle->mac, frame, size_bytes));
}

/** @copydoc neon_eth_process */
neon_status_t neon_eth_process(neon_eth_handle_t *handle, uint32_t *next_poll_ms) {
    if (!eth_valid(handle) || next_poll_ms == NULL) {
        return NEON_INVALID_PARAM;
    }
    /* Supports polling with the same take/ack ordering as the IRQ path. */
    if (handle->mac.state == FX1_ETH_RUNNING) {
        uint32_t enabled = eth_mask_irq();
        fx1_eth_isr(&handle->mac);
        eth_restore_irq(enabled);
    }
    *next_poll_ms = (uint32_t)fx1_eth_service(&handle->mac);
    return handle->mac.state == FX1_ETH_FAULTED ? NEON_ERROR : NEON_OK;
}

/** @copydoc neon_eth_irq_handler */
neon_status_t neon_eth_irq_handler(neon_eth_handle_t *handle) {
    if (!eth_valid(handle)) {
        return NEON_INVALID_PARAM;
    }
    fx1_eth_isr(&handle->mac);
    ++handle->interrupts;
    return NEON_OK;
}

/** @copydoc neon_eth_stop */
neon_status_t neon_eth_stop(neon_eth_handle_t *handle, uint32_t drain_us) {
    if (!eth_valid(handle)) {
        return NEON_INVALID_PARAM;
    }
    uint32_t enabled = eth_mask_irq();
    int result = fx1_eth_stop(&handle->mac, drain_us);
    eth_restore_irq(enabled);
    return eth_status(result);
}

/** @copydoc neon_eth_recover */
neon_status_t neon_eth_recover(neon_eth_handle_t *handle) {
    if (!eth_valid(handle)) {
        return NEON_INVALID_PARAM;
    }
    uint32_t enabled = eth_mask_irq();
    int result = fx1_eth_recover(&handle->mac);
    eth_restore_irq(enabled);
    return eth_status(result);
}

/** @copydoc neon_eth_get_state */
neon_eth_state_t neon_eth_get_state(const neon_eth_handle_t *handle) {
    if (!eth_valid(handle)) {
        return NEON_ETH_STATE_RESET;
    }
    switch (handle->mac.state) {
        case FX1_ETH_UNINIT:
        case FX1_ETH_STOPPED:
            return NEON_ETH_STATE_READY;
        case FX1_ETH_RUNNING:
            return NEON_ETH_STATE_RUNNING;
        case FX1_ETH_STOPPING:
            return NEON_ETH_STATE_STOPPING;
        default:
            return NEON_ETH_STATE_FAULTED;
    }
}

/** @copydoc neon_eth_get_stats */
neon_status_t neon_eth_get_stats(const neon_eth_handle_t *handle, neon_eth_stats_t *stats) {
    if (!eth_valid(handle) || stats == NULL) {
        return NEON_INVALID_PARAM;
    }
    stats->tx_frames = handle->mac.c.tx_frames;
    stats->rx_frames = handle->mac.c.rx_frames;
    stats->tx_errors = handle->mac.c.tx_desc_errors;
    stats->rx_errors = handle->mac.c.rx_desc_errors;
    stats->tx_dropped = handle->mac.c.tx_dropped;
    stats->interrupts = handle->interrupts;
    stats->late_writebacks = handle->mac.c.wb_timeouts;
    stats->recoveries = handle->mac.c.recoveries;
    return NEON_OK;
}

/** @copydoc neon_eth_deinit */
neon_status_t neon_eth_deinit(neon_eth_handle_t *handle) {
    if (!eth_valid(handle)) {
        return NEON_INVALID_PARAM;
    }
    if (handle->mac.state != FX1_ETH_UNINIT && handle->mac.state != FX1_ETH_STOPPED) {
        return NEON_BUSY;
    }
    /* Prevent a stale pending PLIC delivery from touching a released handle. */
    SDK_IRQ_Disable((int32_t)FX1_IRQ_ETH);
    if (handle->mac.state == FX1_ETH_STOPPED) {
        /* Both engines have acknowledged HALTED. Cold init on the next
         * acquisition expects reset IDLE rather than the stopped state. */
        fx1_write32(FX1_ETH_CSR_BASE + ETH_CTRL, 0U);
        fx1_mb();
    }
    handle->acquired = 0U;
    return NEON_OK;
}

/*==================================================================================================
*                                        PRIVATE FUNCTIONS
==================================================================================================*/
static int eth_valid(const neon_eth_handle_t *handle) {
    return handle == &eth_handle && eth_handle.acquired != 0U && fx1_hartid() == 0U;
}

static neon_status_t eth_status(int result) {
    switch (result) {
        case ETH_OK:
            return NEON_OK;
        case ETH_E_STATE:
        case ETH_E_FULL:
            return NEON_BUSY;
        case ETH_E_TIMEOUT:
            return NEON_TIMEOUT;
        case ETH_E_LEN:
        case ETH_E_CFG:
            return NEON_INVALID_PARAM;
        default:
            return NEON_ERROR;
    }
}

static uint32_t eth_mask_irq(void) {
    uint32_t saved = SDK_IRQ_EnterCritical();
    uint32_t enabled = fx1_read32(FX1_PLIC_BASE + FX1_PLIC_ENABLE_OFF) & (1UL << FX1_IRQ_ETH);
    SDK_IRQ_Disable((int32_t)FX1_IRQ_ETH);
    SDK_IRQ_ExitCritical(saved);
    return enabled;
}

static void eth_restore_irq(uint32_t enabled) {
    if (enabled != 0U) {
        SDK_IRQ_Enable((int32_t)FX1_IRQ_ETH);
    }
}
