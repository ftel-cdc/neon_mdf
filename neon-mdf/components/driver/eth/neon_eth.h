/**
 * @file neon_eth.h
 * @brief Ethernet MAC frame I/O, independent of an OS or TCP/IP stack.
 * @details One serialized owner on hart 0 calls every API except irq_handler.
 * RX callbacks run inside process(), borrow the frame until return and must not
 * reenter the driver. TX copies the frame before returning. No heap allocation.
 * FX1 currently provides one instance, fixed 16-entry rings and 2048-byte slots.
 */
#ifndef __NEON_ETH_H__
#define __NEON_ETH_H__

/*==================================================================================================
*                                          INCLUDE FILES
==================================================================================================*/
#include "neon_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==================================================================================================
*                                  STRUCTURES AND OTHER TYPEDEFS
==================================================================================================*/
typedef struct neon_eth_handle_s neon_eth_handle_t;

typedef enum { NEON_ETH0 = 0 } neon_eth_t;

typedef enum {
    NEON_ETH_STATE_RESET = 0,
    NEON_ETH_STATE_READY,
    NEON_ETH_STATE_RUNNING,
    NEON_ETH_STATE_STOPPING,
    NEON_ETH_STATE_FAULTED
} neon_eth_state_t;

typedef struct {
    uint8_t mac_address[6];
    uint32_t speed_mbps;   /**< 10 or 100, full duplex. */
    uint32_t phy_loopback; /**< 0 or 1; loopback does not require a line peer. */
} neon_eth_config_t;

typedef struct {
    uint32_t mac_id;
    uint32_t build_config;
    uint16_t phy_id1;
    uint16_t phy_id2;
    uint32_t link_up;
} neon_eth_info_t;

typedef struct {
    uint32_t tx_frames; /**< Frames submitted, not proof of delivery on the line. */
    uint32_t rx_frames;
    uint32_t tx_errors;
    uint32_t rx_errors;
    uint32_t tx_dropped;
    uint32_t interrupts;
    uint32_t late_writebacks;
    uint32_t recoveries;
} neon_eth_stats_t;

typedef void (*neon_eth_rx_callback_t)(void *context, const uint8_t *frame, uint32_t size_bytes,
                                       uint32_t descriptor_status);

/*==================================================================================================
*                                       FUNCTION PROTOTYPES
==================================================================================================*/
/**
 * @brief Acquire a driver-owned handle with default settings, without hardware access.
 * @param[out] handle Receives the opaque handle. Valid until successful deinit.
 * @param[in] instance MAC instance, currently NEON_ETH0.
 * @return NEON_OK, NEON_BUSY if already acquired, or NEON_INVALID_PARAM.
 */
neon_status_t neon_eth_init(neon_eth_handle_t **handle, neon_eth_t instance);
/**
 * @brief Apply MAC/speed/loopback settings before the first start.
 * @param[in,out] handle Acquired handle.
 * @param[in] config Unicast, nonzero MAC and 10/100 Mbps; copied by driver.
 * @return NEON_OK, NEON_BUSY after start (deinit/init to reconfigure), or NEON_INVALID_PARAM.
 */
neon_status_t neon_eth_set_config(neon_eth_handle_t *handle, const neon_eth_config_t *config);
/**
 * @brief Register an RX callback; NULL disables delivery to the application.
 * @param[in,out] handle Acquired handle.
 * @param[in] callback Receives DA..payload without FCS and raw RX DES0 status.
 * @param[in] context Opaque application data, retained until replaced/deinit.
 * @return NEON_OK or NEON_INVALID_PARAM.
 */
neon_status_t neon_eth_register_callback(neon_eth_handle_t *handle, neon_eth_rx_callback_t callback, void *context);
/**
 * @brief Read MAC identity and PHY over bounded, polled MDIO transactions.
 * @param[in,out] handle Acquired handle.
 * @param[out] info Receives identity and link snapshot; invalid on error.
 * @return NEON_OK, NEON_BUSY in FAULTED state, NEON_ERROR or NEON_INVALID_PARAM.
 */
neon_status_t neon_eth_probe(neon_eth_handle_t *handle, neon_eth_info_t *info);
/**
 * @brief Start/restart descriptor DMA. DDR must already be initialized.
 * @param[in,out] handle Acquired handle; FAULTED requires recover first.
 * @return NEON_OK, NEON_BUSY, NEON_TIMEOUT, NEON_ERROR or NEON_INVALID_PARAM.
 * @note Bounded waits, task/main context only. Driver preserves the PLIC enable.
 */
neon_status_t neon_eth_start(neon_eth_handle_t *handle);
/**
 * @brief Copy and queue one frame; completion is asynchronous.
 * @param[in,out] handle Running handle.
 * @param[in] frame DA..payload, no FCS; copied before return.
 * @param[in] size_bytes 60..1514 bytes.
 * @return NEON_OK, NEON_BUSY if full/halted, or NEON_INVALID_PARAM.
 */
neon_status_t neon_eth_transmit(neon_eth_handle_t *handle, const uint8_t *frame, uint32_t size_bytes);
/**
 * @brief Service completion/errors, reclaim TX and deliver RX callbacks.
 * @param[in,out] handle Acquired handle.
 * @param[out] next_poll_ms Maximum delay before the next call even without IRQ;
 *             zero only when not running. Polling callers may call more often.
 * @return NEON_OK, NEON_ERROR if FAULTED, or NEON_INVALID_PARAM.
 * @note Call after IRQ notification and after TX; never from ISR.
 */
neon_status_t neon_eth_process(neon_eth_handle_t *handle, uint32_t *next_poll_ms);
/**
 * @brief Take/ack MAC status and mask active level errors, without waiting.
 * @param[in,out] handle Acquired handle.
 * @return NEON_OK or NEON_INVALID_PARAM.
 * @note Call in the application's PLIC handler before PLIC complete, or from
 * process() in polling mode. Does not install a trap or enable CPU/PLIC IRQs.
 */
neon_status_t neon_eth_irq_handler(neon_eth_handle_t *handle);
/**
 * @brief Drain, halt DMA and stop the MAC before returning buffer ownership.
 * @param[in,out] handle Acquired handle.
 * @param[in] drain_us TX drain budget in microseconds; zero skips drain only.
 * @return NEON_OK when quiescent, NEON_TIMEOUT/NEON_BUSY if FAULTED, or NEON_INVALID_PARAM.
 * @warning Timeout retains DMA memory/handle. Recover before restart or deinit.
 */
neon_status_t neon_eth_stop(neon_eth_handle_t *handle, uint32_t drain_us);
/**
 * @brief Recover halted directions or retry quiescence from FAULTED.
 * @param[in,out] handle Acquired handle.
 * @return NEON_OK, NEON_TIMEOUT, NEON_ERROR or NEON_INVALID_PARAM.
 * @note Bounded waits, task/main context only; may restart a FAULTED MAC.
 */
neon_status_t neon_eth_recover(neon_eth_handle_t *handle);
/**
 * @brief Get a software state snapshot.
 * @param[in] handle Acquired handle.
 * @return Current state, RESET for an invalid/released handle.
 */
neon_eth_state_t neon_eth_get_state(const neon_eth_handle_t *handle);
/**
 * @brief Read driver counters under the owner's serialization.
 * @param[in] handle Acquired handle.
 * @param[out] stats Counter snapshot.
 * @return NEON_OK or NEON_INVALID_PARAM.
 */
neon_status_t neon_eth_get_stats(const neon_eth_handle_t *handle, neon_eth_stats_t *stats);
/**
 * @brief Release a stopped handle; never resets a live or faulted DMA.
 * @param[in,out] handle Acquired handle.
 * @return NEON_OK, NEON_BUSY until safely stopped, or NEON_INVALID_PARAM.
 */
neon_status_t neon_eth_deinit(neon_eth_handle_t *handle);

#ifdef __cplusplus
}
#endif
#endif /* __NEON_ETH_H__ */
