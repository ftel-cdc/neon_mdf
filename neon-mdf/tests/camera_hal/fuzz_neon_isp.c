/**
 * @file fuzz_neon_isp.c
 * @brief Model-based random test of the ISP HAL on the sim backend: random queue, process, reclaim,
 * acquire, release, start, stop, recover and configure, checked against an independent FIFO model
 * (reclaim order and processed flag, output order, no buffer handed out twice). Usage: fuzz_neon_isp [seeds]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "isp/neon_isp.h"
#include "isp/platform/fx1/fx1_isp_simulation.h"

#define W 64U
#define H 32U
static unsigned long seed;
static unsigned rnd(unsigned n) { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; return (unsigned)((seed >> 33) % n); }
static int fails;
#define BUG(...) do { printf("BUG seed=%lu step=%d: ", s0, step); printf(__VA_ARGS__); printf("\n"); fails++; return; } while (0)

enum { PENDING, PROCESSED, DISCARDED };
typedef struct { uint32_t tag; int st; } ent_t;
static ent_t in_q[64]; static int in_n;           /* queued, not reclaimed, FIFO */
static uint32_t out_q[64]; static int out_n;      /* completed, not acquired, FIFO */
static uint8_t held[8]; static uint32_t held_tag[8]; static int held_n;

static void cfg(neon_isp_config_t *c) {
    neon_isp_get_default_config(c);
    c->width = W; c->height = H; c->input_stride_bytes = 2*W; c->y_stride_bytes = W; c->uv_stride_bytes = W;
    c->output_y_bytes = W*H; c->output_uv_bytes = W*H/2;
    for (unsigned i = 0; i < NEON_ISP_MAX_BUFFERS; i++) { c->output_y_addr[i] = 0x90000000u + i*0x2000u; c->output_uv_addr[i] = 0x91000000u + i*0x2000u; }
}

static void run(unsigned long s0) {
    neon_isp_handle_t isp; neon_isp_config_t c; uint32_t next_tag = 1; int step = 0;
    seed = s0; in_n = out_n = held_n = 0;
    neon_isp_init(&isp); cfg(&c);
    if (neon_isp_configure(&isp, &c) != NEON_OK) BUG("configure");
    for (step = 0; step < 400; step++) {
        unsigned op = rnd(100);
        neon_status_t r;
        if (op < 25) {                     /* queue */
            neon_isp_input_t in; memset(&in, 0, sizeof in);
            in.address = 0x80000000u + (next_tag % 16u) * 0x2000u; in.size_bytes = 2*W*H; in.tag = next_tag;
            r = neon_isp_queue_input(&isp, &in);
            if (r == NEON_OK) { in_q[in_n].tag = next_tag; in_q[in_n].st = PENDING; in_n++; }
            else if (r == NEON_BUSY) { if (in_n < 4 && !isp.input_flushing) BUG("queue busy with %d queued", in_n); }
            else if (isp.state != NEON_ISP_STATE_IDLE) BUG("queue -> %d", r);
            next_tag++;
        } else if (op < 45) {              /* process */
            unsigned k = rnd(3) + 1; uint32_t d = fx1_isp_sim_process(&isp, k);
            for (int i = 0; i < in_n && d > 0; i++) if (in_q[i].st == PENDING) { in_q[i].st = PROCESSED; out_q[out_n++] = in_q[i].tag; d--; }
            if (d) BUG("sim processed a frame the model did not have");
        } else if (op < 60) {              /* reclaim */
            neon_isp_input_t back; r = neon_isp_reclaim_input(&isp, &back);
            if (r == NEON_OK) {
                if (in_n == 0) BUG("reclaim gave a frame from an empty queue");
                if (back.tag != in_q[0].tag) BUG("reclaim order: got %u want %u", back.tag, in_q[0].tag);
                if (in_q[0].st == PENDING) BUG("reclaimed tag %u that is still pending", back.tag);
                if (back.processed != (in_q[0].st == PROCESSED)) BUG("processed flag wrong for %u (model %d)", back.tag, in_q[0].st);
                memmove(in_q, in_q + 1, (size_t)(--in_n) * sizeof in_q[0]);
            } else if (r == NEON_BUSY) { if (in_n && in_q[0].st != PENDING) BUG("reclaim busy but head %u is done", in_q[0].tag); }
            else if (isp.state != NEON_ISP_STATE_IDLE) BUG("reclaim -> %d", r);
        } else if (op < 72) {              /* acquire */
            neon_isp_output_t o; r = neon_isp_acquire_output(&isp, &o);
            if (r == NEON_OK) {
                if (out_n == 0) BUG("acquire without a completed frame");
                if (o.tag != out_q[0]) BUG("acquire order: got %u want %u", o.tag, out_q[0]);
                for (int i = 0; i < held_n; i++) if (held[i] == o.buffer_index) BUG("buffer %u handed out twice", o.buffer_index);
                held[held_n] = o.buffer_index; held_tag[held_n++] = o.tag;
                memmove(out_q, out_q + 1, (size_t)(--out_n) * sizeof out_q[0]);
            } else if (r == NEON_BUSY) { if (out_n) BUG("acquire busy with %d completed", out_n); }
            else if (isp.state == NEON_ISP_STATE_RUNNING) BUG("acquire -> %d", r);
        } else if (op < 84) {              /* release */
            if (held_n) { int i = (int)rnd((unsigned)held_n);
                r = neon_isp_release_output(&isp, held[i]);
                if (r != NEON_OK) BUG("release %u -> %d", held[i], r);
                held[i] = held[--held_n]; held_tag[i] = held_tag[held_n]; }
        } else if (op < 89) {              /* start */
            r = neon_isp_start(&isp);
            if (r == NEON_OK) { held_n = 0; }
        } else if (op < 94) {              /* stop */
            r = neon_isp_stop(&isp);
            if (r == NEON_OK) { for (int i = 0; i < in_n; i++) if (in_q[i].st == PENDING) in_q[i].st = DISCARDED; out_n = 0; held_n = 0; }
        } else if (op < 97) {              /* recover */
            r = neon_isp_recover(&isp, NEON_ISP_RECOVER_SOFT);
            if (r == NEON_OK) { for (int i = 0; i < in_n; i++) if (in_q[i].st == PENDING) in_q[i].st = DISCARDED; out_n = 0; held_n = 0; }
        } else {                           /* configure */
            r = neon_isp_configure(&isp, &c);
            if (r == NEON_OK) { in_n = out_n = held_n = 0; }
        }
    }
}

int main(int argc, char **argv) {
    unsigned long n = argc > 1 ? strtoul(argv[1], 0, 0) : 20000;
    for (unsigned long s = 1; s <= n && fails < 5; s++) run(s);
    printf("isp fuzz: %lu seeds, %d bugs\n", n, fails);
    return fails != 0;
}
