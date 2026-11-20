/** @file ftel_isp_auto_private.h @internal Binary integration workspace. */
#ifndef FTEL_ISP_AUTO_PRIVATE_H
#define FTEL_ISP_AUTO_PRIVATE_H
#include "ftel_isp_auto.h"
#include "fpt_3a.h"
#define FTEL_AUTO_MAGIC 0x4155544fu
typedef struct {
    uint32_t magic, recovery_sequence, token, last_frame, have_frame;
    uint16_t width, height;
    bool aec_ready, awb_ready, recovery_bound;
    fpt_3a_info_t info;
    fpt_3a_stats_t stats;
    fpt_3a_aec_zone_t aec[FPT_3A_MAX_ZONES];
    fpt_3a_awb_zone_t awb[FPT_3A_MAX_ZONES];
    uint32_t histogram[FPT_3A_HIST_BINS];
} ftel_auto_workspace_t;
ftel_isp_status_t ftel_isp_auto_collect(ftel_auto_workspace_t *workspace);
#endif
