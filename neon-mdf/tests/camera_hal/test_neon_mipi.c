/**
 * @file test_neon_mipi.c
 * @brief Host tests of the MIPI D-PHY link HAL on the in-RAM backend.
 */

#include "test_common.h"
#include "mipi/neon_mipi.h"
#include "mipi/platform/fx1/fx1_mipi_simulation.h"

#define POLLS 8U

static neon_mipi_config_t good_config(void)
{
    neon_mipi_config_t config = {NEON_MIPI_DIRECTION_RX, 4U, 1000000000UL, true};

    return config;
}

static void test_init_and_arguments(void)
{
    neon_mipi_handle_t link;
    neon_mipi_caps_t caps;
    neon_mipi_config_t config = good_config();

    CHECK_EQ(neon_mipi_init(NULL), NEON_INVALID_PARAM);
    CHECK_EQ(neon_mipi_init(&link), NEON_OK);
    CHECK_EQ(link.state, NEON_MIPI_STATE_RELEASED);
    CHECK_EQ(neon_mipi_get_caps(&caps), NEON_OK);
    CHECK_EQ(caps.max_lanes, 4U);
    CHECK_EQ(caps.lane_count_mask, 0x16U); /* 1, 2 and 4 lanes. */
    CHECK_EQ(caps.max_lane_rate_bps, 1500000000UL);
    CHECK(caps.rx_supported);
    CHECK(!caps.tx_supported);
    CHECK(!caps.non_continuous_clock);
    CHECK_EQ(neon_mipi_acquire(NULL, &config), NEON_INVALID_PARAM);
    CHECK_EQ(neon_mipi_acquire(&link, NULL), NEON_INVALID_PARAM);
    CHECK_EQ(neon_mipi_wait_ready(&link, 0U), NEON_INVALID_PARAM);
    CHECK_EQ(neon_mipi_wait_ready(&link, POLLS), NEON_ERROR); /* Nothing acquired. */
    CHECK_EQ(neon_mipi_release(&link), NEON_ERROR);
    CHECK_EQ(neon_mipi_lock(&link), NEON_ERROR);
    CHECK_EQ(neon_mipi_unlock(&link), NEON_ERROR);
    CHECK_EQ(neon_mipi_deinit(&link), NEON_OK);
}

static void test_config_validation(void)
{
    neon_mipi_handle_t link;
    neon_mipi_config_t config;
    neon_mipi_caps_t caps;

    (void)neon_mipi_init(&link);
    CHECK_EQ(neon_mipi_get_caps(&caps), NEON_OK);

    config = good_config();
    config.direction = NEON_MIPI_DIRECTION_TX;
    CHECK_EQ(neon_mipi_acquire(&link, &config), NEON_NOT_SUPPORTED);
    config = good_config();
    config.lane_count = 0U;
    CHECK_EQ(neon_mipi_acquire(&link, &config), NEON_INVALID_PARAM);
    config.lane_count = 5U;
    CHECK_EQ(neon_mipi_acquire(&link, &config), NEON_INVALID_PARAM);
    config.lane_count = 3U;
    CHECK_EQ(neon_mipi_acquire(&link, &config), NEON_NOT_SUPPORTED);
    config = good_config();
    config.lane_rate_bps = 0U;
    CHECK_EQ(neon_mipi_acquire(&link, &config), NEON_INVALID_PARAM);
    config.lane_rate_bps = caps.max_lane_rate_bps + 1U;
    CHECK_EQ(neon_mipi_acquire(&link, &config), NEON_NOT_SUPPORTED);
    config = good_config();
    config.continuous_clock = false;
    CHECK_EQ(neon_mipi_acquire(&link, &config), NEON_NOT_SUPPORTED);
    CHECK_EQ(link.state, NEON_MIPI_STATE_RELEASED); /* A rejected configuration leaves no trace. */

    config = good_config();
    config.lane_rate_bps = caps.max_lane_rate_bps;
    CHECK_EQ(neon_mipi_acquire(&link, &config), NEON_OK);
    CHECK_EQ(neon_mipi_acquire(&link, &config), NEON_BUSY);
}

static void test_bring_up_and_timeout(void)
{
    neon_mipi_handle_t link;
    neon_mipi_config_t config = good_config();
    neon_mipi_status_t status;

    config.lane_count = 2U;
    (void)neon_mipi_init(&link);
    fx1_mipi_sim_set_init_polls(&link, 5U);
    CHECK_EQ(neon_mipi_acquire(&link, &config), NEON_OK);
    CHECK_EQ(link.state, NEON_MIPI_STATE_ENABLED);
    CHECK_EQ(neon_mipi_lock(&link), NEON_ERROR); /* Not ready yet. */
    CHECK_EQ(neon_mipi_wait_ready(&link, 2U), NEON_TIMEOUT);
    CHECK_EQ(link.state, NEON_MIPI_STATE_ENABLED);
    CHECK_EQ(fx1_mipi_sim_get_active_lanes(&link), 0U); /* Lanes are applied only after Stop State. */
    CHECK_EQ(neon_mipi_wait_ready(&link, POLLS), NEON_OK);
    CHECK_EQ(link.state, NEON_MIPI_STATE_READY);
    CHECK_EQ(fx1_mipi_sim_get_active_lanes(&link), 2U);
    CHECK_EQ(neon_mipi_wait_ready(&link, 1U), NEON_OK); /* Already ready. */

    CHECK_EQ(neon_mipi_get_status(&link, &status), NEON_OK);
    CHECK_EQ(status.state, NEON_MIPI_STATE_READY);
    CHECK_EQ(status.lane_count, 2U);
    CHECK_EQ(status.lane_rate_bps, 1000000000UL);
    CHECK(status.clock_stop_state);
    CHECK_EQ(status.lane_stop_mask, 0x3U);
    CHECK(!status.clock_hs_active);
}

static void test_stuck_lane(void)
{
    neon_mipi_handle_t link;
    neon_mipi_config_t config = good_config();

    (void)neon_mipi_init(&link);
    fx1_mipi_sim_set_stuck_lanes(&link, 0x4U);
    CHECK_EQ(neon_mipi_acquire(&link, &config), NEON_OK);
    CHECK_EQ(neon_mipi_wait_ready(&link, POLLS), NEON_TIMEOUT);
    CHECK_EQ(link.state, NEON_MIPI_STATE_ENABLED);
    fx1_mipi_sim_set_stuck_lanes(&link, 0U);
    CHECK_EQ(neon_mipi_wait_ready(&link, POLLS), NEON_OK);
}

static void test_lock_and_release(void)
{
    neon_mipi_handle_t link;
    neon_mipi_config_t config = good_config();
    neon_mipi_status_t status;

    (void)neon_mipi_init(&link);
    (void)neon_mipi_acquire(&link, &config);
    (void)neon_mipi_wait_ready(&link, POLLS);
    CHECK_EQ(neon_mipi_lock(&link), NEON_OK);
    CHECK_EQ(neon_mipi_lock(&link), NEON_OK);
    (void)neon_mipi_get_status(&link, &status);
    CHECK_EQ(status.lock_count, 2U);
    CHECK_EQ(neon_mipi_release(&link), NEON_LOCKED);
    CHECK_EQ(neon_mipi_deinit(&link), NEON_BUSY);
    CHECK_EQ(neon_mipi_unlock(&link), NEON_OK);
    CHECK_EQ(neon_mipi_release(&link), NEON_LOCKED);
    CHECK_EQ(neon_mipi_unlock(&link), NEON_OK);
    CHECK_EQ(neon_mipi_unlock(&link), NEON_ERROR);
    CHECK_EQ(neon_mipi_release(&link), NEON_OK);
    CHECK_EQ(link.state, NEON_MIPI_STATE_RELEASED);
    CHECK_EQ(neon_mipi_deinit(&link), NEON_OK);
}

static void test_recover_levels(void)
{
    neon_mipi_handle_t link;
    neon_mipi_config_t config = good_config();
    fx1_mipi_sim_counters_t counters;

    (void)neon_mipi_init(&link);
    CHECK_EQ(neon_mipi_recover(&link, NEON_MIPI_RECOVER_PHY, POLLS), NEON_ERROR); /* Released. */
    (void)neon_mipi_acquire(&link, &config);
    (void)neon_mipi_wait_ready(&link, POLLS);
    CHECK_EQ(neon_mipi_recover(&link, NEON_MIPI_RECOVER_PHY, 0U), NEON_INVALID_PARAM);
    CHECK_EQ(neon_mipi_recover(&link, (neon_mipi_recover_level_t)9, POLLS), NEON_INVALID_PARAM);

    CHECK_EQ(neon_mipi_recover(&link, NEON_MIPI_RECOVER_PHY, POLLS), NEON_OK);
    CHECK_EQ(link.phy_epoch, 1U);
    CHECK_EQ(link.config_epoch, 0U);
    CHECK_EQ(link.state, NEON_MIPI_STATE_READY);
    CHECK_EQ(neon_mipi_recover(&link, NEON_MIPI_RECOVER_FULL, POLLS), NEON_OK);
    CHECK_EQ(link.phy_epoch, 2U);
    CHECK_EQ(link.config_epoch, 1U);
    CHECK_EQ(link.state, NEON_MIPI_STATE_READY);
    CHECK_EQ(fx1_mipi_sim_get_active_lanes(&link), 4U); /* Lanes are applied again after the reset. */
    fx1_mipi_sim_get_counters(&link, &counters);
    CHECK_EQ(counters.phy_resets, 2U); /* The acquire sequence starts with dphy_rst_req, then the recovery. */
    CHECK_EQ(counters.full_resets, 1U);

    /* A recovery that cannot reach Stop State reports it and leaves the link not ready. */
    fx1_mipi_sim_set_stuck_lanes(&link, 0x1U);
    CHECK_EQ(neon_mipi_recover(&link, NEON_MIPI_RECOVER_PHY, POLLS), NEON_TIMEOUT);
    CHECK_EQ(link.state, NEON_MIPI_STATE_ENABLED);
}

static void test_high_speed_and_errors(void)
{
    neon_mipi_handle_t link;
    neon_mipi_config_t config = good_config();
    neon_mipi_status_t status;

    (void)neon_mipi_init(&link);
    (void)neon_mipi_acquire(&link, &config);
    (void)neon_mipi_wait_ready(&link, POLLS);
    fx1_mipi_sim_set_hs_active(&link, true);
    (void)neon_mipi_get_status(&link, &status);
    CHECK(status.clock_hs_active);
    CHECK(!status.clock_stop_state);
    fx1_mipi_sim_inject_lane_errors(&link, 0x1U, 0x2U, 3U);
    (void)neon_mipi_get_status(&link, &status);
    CHECK_EQ(status.sot_error_mask, 0x1U);
    CHECK_EQ(status.sot_sync_error_mask, 0x2U);
    CHECK_EQ(status.lane_abort_count, 3U);
    CHECK_EQ(neon_mipi_recover(&link, NEON_MIPI_RECOVER_PHY, POLLS), NEON_OK);
    (void)neon_mipi_get_status(&link, &status);
    CHECK_EQ(status.sot_error_mask, 0U); /* A PHY reset clears the lane errors and leaves High-Speed mode. */
    CHECK(!status.clock_hs_active);
}

int main(void)
{
    RUN(test_init_and_arguments);
    RUN(test_config_validation);
    RUN(test_bring_up_and_timeout);
    RUN(test_stuck_lane);
    RUN(test_lock_and_release);
    RUN(test_recover_levels);
    RUN(test_high_speed_and_errors);
    return TEST_REPORT();
}
