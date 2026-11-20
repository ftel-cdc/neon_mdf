/**
 * @file main.c
 * @brief Check Neon ETH on FX1: polling, PLIC, ring wraps and stop/restart.
 */
#include "neon.h"
#include "fx1_fw.h"
#include "sdk_irq.h"
#include <stddef.h>

static neon_eth_handle_t *eth;
static uint8_t frame[1514];
static uint32_t checks;
static uint32_t received;
static uint32_t bad_frames;
static uint32_t expected_size;
static volatile uint32_t irq_count;
static const uint8_t mac[6] = {0x02U, 0x00U, 0x46U, 0x58U, 0x31U, 0x01U};

static void check(int ok, const char *name) {
    ++checks;
    fx1_puts(ok ? "[NEON_ETH] PASS " : "[NEON_ETH] FAIL ");
    fx1_putdec(checks);
    fx1_puts(" ");
    fx1_puts(name);
    fx1_putc('\n');
    if (!ok) {
        fx1_fail(0xe000U + checks);
    }
}

static void receive_frame(void *context, const uint8_t *data, uint32_t size_bytes, uint32_t status) {
    (void)context;
    ++received;
    if (size_bytes != expected_size || (status & 0x00070000U) != 0U) {
        ++bad_frames;
        return;
    }
    for (uint32_t i = 0U; i < size_bytes; ++i) {
        if (data[i] != frame[i]) {
            ++bad_frames;
            return;
        }
    }
}

static uint32_t trap_handler(uint32_t cause, uint32_t pc, uint32_t value) {
    if (cause == (0x80000000U | FX1_IRQ_M_EXT)) {
        uint32_t source = fx1_plic_claim(FX1_PLIC_CONTEXT_HART_M(0U));
        if (source == FX1_IRQ_ETH) {
            neon_eth_irq_handler(eth);
            ++irq_count;
        } else {
            fx1_fail(0xe100U + source);
        }
        fx1_plic_complete(FX1_PLIC_CONTEXT_HART_M(0U), source);
        return pc;
    }
    fx1_log_hex("unexpected trap", cause);
    fx1_log_hex("mtval", value);
    fx1_fail(0xe200U);
}

static void exchange(uint32_t sequence, uint32_t size_bytes) {
    expected_size = size_bytes;
    for (uint32_t i = 0U; i < size_bytes; ++i) {
        frame[i] = (uint8_t)(i * 37U + sequence * 13U);
    }
    for (uint32_t i = 0U; i < 6U; ++i) {
        frame[i] = mac[i];
        frame[6U + i] = mac[i];
    }
    frame[12] = 0x88U;
    frame[13] = 0xb5U;
    uint32_t target = received + 1U;
    check(neon_eth_transmit(eth, frame, size_bytes) == NEON_OK, "queue TX");
    uint64_t deadline = fx1_mtime() + FX1_MTIME_HZ / 5U;
    uint32_t next_poll_ms;
    do {
        if (neon_eth_process(eth, &next_poll_ms) != NEON_OK) {
            fx1_fail(0xe300U);
        }
    } while (received < target && fx1_mtime() < deadline);
    check(received == target && bad_frames == 0U, "RX payload matches every byte");
}

int main(void) {
    neon_eth_info_t info;
    neon_eth_stats_t stats;
    neon_eth_handle_t *duplicate = NULL;
    neon_eth_config_t config = {
        .mac_address = {0x02U, 0x00U, 0x46U, 0x58U, 0x31U, 0x01U}, .speed_mbps = 100U, .phy_loopback = 1U};
    check(neon_eth_init(&eth, NEON_ETH0) == NEON_OK, "acquire ETH0");
    check(neon_eth_init(&duplicate, NEON_ETH0) == NEON_BUSY, "reject duplicate owner");
    check(neon_eth_transmit(eth, NULL, 60U) == NEON_INVALID_PARAM, "reject NULL TX");
    config.speed_mbps = 1000U;
    check(neon_eth_set_config(eth, &config) == NEON_INVALID_PARAM, "reject unsupported speed");
    config.speed_mbps = 100U;
    check(neon_eth_set_config(eth, &config) == NEON_OK, "configure 100 Mbps PHY loopback");
    check(neon_eth_register_callback(eth, receive_frame, NULL) == NEON_OK, "register RX callback");
    check(neon_eth_probe(eth, &info) == NEON_OK && info.mac_id == 0x45544801U && info.phy_id1 == 0x0007U &&
              info.phy_id2 == 0xc0f1U,
          "MAC and MDIO PHY identity");
    check(neon_eth_start(eth) == NEON_OK, "start MAC/DMA");
    check(neon_eth_deinit(eth) == NEON_BUSY, "retain live DMA handle");
    check(neon_eth_set_config(eth, &config) == NEON_BUSY, "reject live reconfiguration");
    const uint32_t sizes[] = {60U, 61U, 512U, 1514U};
    for (uint32_t i = 0U; i < 40U; ++i) {
        exchange(i, sizes[i % 4U]);
    }
    check(irq_count == 0U, "polling works with PLIC source disabled");
    fx1_set_trap_handler(trap_handler);
    fx1_plic_set_threshold(FX1_PLIC_CONTEXT_HART_M(0U), 0U);
    SDK_IRQ_SetPriority((int32_t)FX1_IRQ_ETH, 2U);
    SDK_IRQ_Enable((int32_t)FX1_IRQ_ETH);
    fx1_irq_enable(1UL << FX1_IRQ_M_EXT);
    fx1_global_irq_enable();
    exchange(40U, 1514U);
    check(irq_count > 0U, "PLIC source 6 invokes Neon IRQ handler");
    check(neon_eth_stop(eth, 20000U) == NEON_OK, "quiescent stop");
    check(neon_eth_stop(eth, 0U) == NEON_OK, "repeat stop");
    check(neon_eth_start(eth) == NEON_OK, "restart rings");
    exchange(41U, 61U);
    check(neon_eth_stop(eth, 20000U) == NEON_OK, "stop after restart");
    check(neon_eth_get_stats(eth, &stats) == NEON_OK && stats.tx_frames == 42U && stats.rx_frames == 42U &&
              stats.tx_errors == 0U && stats.rx_errors == 0U,
          "42 TX/RX frames without descriptor errors");
    check(neon_eth_deinit(eth) == NEON_OK, "release stopped handle");
    check(neon_eth_get_state(eth) == NEON_ETH_STATE_RESET, "released handle invalid");
    check(neon_eth_init(&eth, NEON_ETH0) == NEON_OK, "reacquire for 10 Mbps");
    config.speed_mbps = 10U;
    check(neon_eth_set_config(eth, &config) == NEON_OK, "configure 10 Mbps");
    check(neon_eth_register_callback(eth, receive_frame, NULL) == NEON_OK, "restore callback");
    check(neon_eth_start(eth) == NEON_OK, "start 10 Mbps");
    exchange(42U, 512U);
    check(neon_eth_stop(eth, 20000U) == NEON_OK && neon_eth_deinit(eth) == NEON_OK, "stop and release 10 Mbps");
    fx1_puts("[NEON_ETH] SUMMARY checks=");
    fx1_putdec(checks);
    fx1_puts(" failed=0\n");
    fx1_pass();
}
