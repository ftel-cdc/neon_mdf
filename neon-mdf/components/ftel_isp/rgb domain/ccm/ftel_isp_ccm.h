/*
 * Cấu hình ma trận hiệu chỉnh màu có dấu 3x3 và offset RGB.
 * Khai báo API và cấu trúc dữ liệu.
 */
#ifndef FTEL_ISP_CCM_H
#define FTEL_ISP_CCM_H
#include "common/ftel_isp_common.h"

/* Hệ số ma trận có dấu SQ2.9 và offset có dấu 12 bit theo đơn vị pixel đầu ra (ISP-UPD-007). */
typedef struct {
    bool enable;
    int16_t matrix[9];
    int16_t offset[3]; /* R/G/B: 1 tương ứng cộng một mức pixel, không phải Q3.9. */
} ccm_config_t;

void ccm_get_default_config(ccm_config_t *cfg);
isp_status_t ccm_validate_config(const ccm_config_t *cfg);
isp_status_t ccm_configure(const ccm_config_t *cfg);

#endif
