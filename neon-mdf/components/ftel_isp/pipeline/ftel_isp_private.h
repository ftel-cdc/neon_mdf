#ifndef FTEL_ISP_FX1_PRIVATE_H
#define FTEL_ISP_FX1_PRIVATE_H

/* Chỉ lớp hiện thực nhìn thấy driver và regmap FX1. */
#include "ftel_isp.h"
#include "platform/fx1/ftel_isp_platform.h"
#include "registers/ftel_isp_regs.h"
#include "bayer domain/ifmt/ftel_isp_ifmt.h"
#include "bayer domain/blc/ftel_isp_blc.h"
#include "bayer domain/bpc/ftel_isp_bpc.h"
#include "bayer domain/wb/ftel_isp_wb.h"
#include "bayer domain/dg/ftel_isp_dg.h"
#include "stats/aec/ftel_isp_aec.h"

typedef struct {
    ifmt_config_t ifmt;
    blc_config_t blc;
    bpc_config_t bpc;
    wb_config_t wb;
    dg_config_t dg;
    aec_config_t aec;
} ftel_isp_fx1_config_t;

typedef struct {
    bool ready, submitted, complete;
    ftel_isp_config_t config;
    uint16_t active_width, active_height;
    uint32_t recovery_sequence, frame_id;
    uint32_t input_before, output_before, tag, timestamp;
    bool recovering;
} ftel_isp_fx1_session_t;

extern ftel_isp_fx1_session_t ftel_isp_fx1_session;
/* Lớp OS giữ quyền truy cập bằng callback; driver thuần không include RTOS. */
bool ftel_isp_fx1_access_allowed(void);
void ftel_isp_fx1_set_owner(bool (*allowed)(void *), void *user);
ftel_isp_status_t ftel_isp_fx1_submit(const ftel_isp_frame_t *frame, bool irq,
                                       uint32_t prepare_timeout_us);
ftel_isp_status_t ftel_isp_fx1_collect(ftel_isp_frame_result_t *result);
ftel_isp_status_t ftel_isp_fx1_recovery_begin(void);
ftel_isp_status_t ftel_isp_fx1_recovery_poll(void);
ftel_isp_status_t ftel_isp_fx1_status(isp_status_t status);

isp_status_t ftel_isp_fx1_prepare_config(const ftel_isp_config_t *config,
                                        ftel_isp_fx1_config_t *backend,
                                        ftel_isp_init_result_t *result);
isp_status_t ftel_isp_fx1_bayer_init(const ftel_isp_fx1_config_t *config,
                                    ftel_isp_init_result_t *result);
isp_status_t ftel_isp_fx1_aec_init(const ftel_isp_fx1_config_t *config,
                                  ftel_isp_init_result_t *result);
#endif
