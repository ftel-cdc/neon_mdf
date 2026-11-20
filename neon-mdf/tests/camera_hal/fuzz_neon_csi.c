/**
 * @file fuzz_neon_csi.c
 * @brief Random test of the CSI HAL on the sim backend, with and without the metadata record, across the
 * 14-bit frame id wrap, soft recoveries and restarts: a buffer is never handed out twice and acquire always
 * returns the oldest completed frame.
 *
 * Without metadata the HAL can only assume that buffers come back in the order they were taken (see
 * neon_csi_acquire_frame()). That combination with out-of-order releases is run and reported as a known
 * limitation, not as a failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "csi/neon_csi.h"
#include "csi/platform/fx1/fx1_csi_simulation.h"
#include "mipi/platform/fx1/fx1_mipi_simulation.h"

static unsigned long seed;
static unsigned rnd(unsigned n) { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; return (unsigned)((seed >> 33) % n); }
static int fails; static long cov[8];
#define BUG(...) do { if (!quiet) { printf("BUG meta=%d inorder=%d seed=%lu step=%d: ", meta, inorder, s0, step); printf(__VA_ARGS__); printf("\n"); } fails++; return; } while (0)

static int quiet;
static void run(unsigned long s0, int meta, int inorder) {
    neon_mipi_handle_t link; neon_csi_handle_t csi; neon_csi_config_t c; neon_mipi_config_t mc = {NEON_MIPI_DIRECTION_RX, 4, 1000000000UL, true};
    uint64_t side_ts[2] = {0, 0}; int ready[2] = {0, 0}, held[2] = {0, 0}; uint64_t ts = 1; int step = 0;
    int held_order[2]; int held_n = 0;
    seed = s0;
    neon_mipi_init(&link); neon_csi_init(&csi, &link);
    neon_mipi_acquire(&link, &mc); neon_mipi_wait_ready(&link, 4);
    neon_csi_get_default_config(&c); c.width = 64; c.height = 32;
    c.dma.base_addr[0] = 0x80000000u; c.dma.base_addr[1] = 0x80010000u; c.dma.buffer_size_bytes = 4096;
    if (meta) { c.metadata.enable = true; c.metadata.data_type = 0x12; c.metadata.area_size_bytes = 256;
                c.metadata.base_addr[0] = 0x80100000u; c.metadata.base_addr[1] = 0x80101000u; }
    if (neon_csi_configure(&csi, &c) != NEON_OK || neon_csi_start(&csi) != NEON_OK) BUG("bring-up");
    fx1_csi_sim_set_next_frame_id(&csi, 16380u + rnd(8)); /* cross the 14-bit wrap */
    for (step = 0; step < 300; step++) {
        unsigned op = rnd(100); neon_status_t r;
        if (op < 35) {
            fx1_csi_sim_frame_t f; neon_csi_status_t st; memset(&f, 0, sizeof f); f.timestamp_ns = ts++;
            f.fault = rnd(10) == 0; f.last_valid_line = 3;
            r = fx1_csi_sim_push_frame(&csi, &f);
            if (r == NEON_OK && neon_csi_get_status(&csi, &st) == NEON_OK) {
                for (int s = 0; s < 2; s++) if (!ready[s] && !held[s] && st.buffer_state[s] != NEON_CSI_BUFFER_HW_OWNED) { ready[s] = 1; side_ts[s] = f.timestamp_ns; }
            }
        } else if (op < 65) {
            neon_csi_frame_t fr; r = neon_csi_acquire_frame(&csi, &fr);
            if (r == NEON_OK) {
                int s = fr.buffer_index, o = 1 - s;
                if (!ready[s] || held[s]) BUG("acquired side %d that is not ready (ready=%d held=%d)", s, ready[s], held[s]);
                if (ready[o] && !held[o] && side_ts[o] < side_ts[s]) { cov[1]++; BUG("not oldest: got side %d ts %llu, side %d holds older ts %llu", s, (unsigned long long)side_ts[s], o, (unsigned long long)side_ts[o]); }
                if (meta && fr.timestamp_ns != side_ts[s]) BUG("context timestamp %llu, frame was %llu", (unsigned long long)fr.timestamp_ns, (unsigned long long)side_ts[s]);
                if (fr.context_valid && fr.timestamp_ns != side_ts[s]) BUG("context_valid with wrong timestamp");
                held[s] = 1; ready[s] = 0; held_order[held_n++] = s; cov[0]++;
            } else if (r == NEON_BUSY) { if ((ready[0] && !held[0]) || (ready[1] && !held[1])) BUG("busy with a ready buffer"); }
            else BUG("acquire -> %d", r);
        } else if (op < 90) {
            if (held_n) { int k = inorder ? 0 : (int)rnd((unsigned)held_n); int s = held_order[k];
                r = neon_csi_release_frame(&csi, (uint8_t)s); if (r != NEON_OK) BUG("release -> %d", r);
                held[s] = 0;
                if (k == 0 && held_n == 2) held_order[0] = held_order[1]; /* Keep the order of acquisition. */
                held_n--; }
        } else if (op < 95) {
            fx1_mipi_sim_set_hs_active(&link, true);
            r = neon_csi_recover(&csi, NEON_CSI_RECOVER_SOFT);
            fx1_mipi_sim_set_hs_active(&link, false);
            if (r != NEON_OK) BUG("recover -> %d", r);
            ready[0] = ready[1] = held[0] = held[1] = 0; held_n = 0; cov[2]++;
        } else {
            if (neon_csi_stop(&csi) != NEON_OK || neon_csi_start(&csi) != NEON_OK) BUG("restart");
            ready[0] = ready[1] = held[0] = held[1] = 0; held_n = 0; cov[3]++;
        }
    }
}

int main(void) {
    int real = 0;
    for (int meta = 0; meta < 2; meta++) for (int inorder = 1; inorder >= 0; inorder--) {
        int before = fails; int known = !meta && !inorder;
        memset(cov, 0, sizeof cov); quiet = known;
        for (unsigned long s = 1; s <= 5000 && fails - before < 3; s++) run(s, meta, inorder);
        printf("csi meta=%d release_in_order=%d: acquires=%ld recovers=%ld restarts=%ld %s=%d\n", meta, inorder,
               cov[0], cov[2], cov[3], known ? "known-limitation hits" : "bugs", fails - before);
        if (!known) real += fails - before;
    }
    return real != 0;
}
