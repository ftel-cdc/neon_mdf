/*
 * Cấu hình bù sáng ống kính và nạp/xác nhận các bảng hệ số lưới lưu trú.
 * Khai báo API và cấu trúc dữ liệu.
 */
#ifndef FTEL_ISP_LSC_H
#define FTEL_ISP_LSC_H

#include "common/ftel_isp_common.h"

/*
 * LSC: bù suy giảm độ sáng ống kính bằng lưới hệ số.
 */

/*
 * Hình dạng lưới dùng chung cho ba bộ lưu trú. Giá trị sau reset 0/0 cố ý không hợp lệ; chỉ đổi
 * sau khi xử lý hết dữ liệu.
 */
typedef struct {
    uint8_t  nx;        /* Số nút ngang 2..32, không vượt active_w. */
    uint8_t  ny;        /* Số nút dọc 2..32, không vượt active_h. */
    uint32_t strength;  /* UQ1.16, 0..0x10000; cực đại áp dụng toàn bộ lưới. */
} lsc_mesh_config_t;

/* Điền giá trị sau reset: lưới 0/0 chưa hợp lệ, cường độ 0. */
void          lsc_get_default_config(lsc_mesh_config_t *cfg);

/*
 * Kiểm tra và ghi cường độ/hình học lưới; lưới không vượt active_w/active_h đã suy ra từ IFMT. Yêu
 * cầu pipeline đã xử lý hết dữ liệu và ở ngoài chế độ profile.
 */
isp_status_t  lsc_validate_config(const lsc_mesh_config_t *cfg,
                                uint16_t active_w, uint16_t active_h);
isp_status_t  lsc_configure(const lsc_mesh_config_t *cfg,
                            uint16_t active_w, uint16_t active_h);

/* Số hệ số mỗi lần nạp lưới = 4 * nx * ny. */
uint32_t      lsc_coef_count(const lsc_mesh_config_t *cfg);

/* Bước đang thực hiện và ảnh chụp trạng thái trước khi hủy một lần nạp lỗi. */
typedef enum {
    LSC_STAGE_PREPARE, LSC_STAGE_CONFIGURE, LSC_STAGE_BEGIN, LSC_STAGE_DATA,
    LSC_STAGE_FULL, LSC_STAGE_VALIDATE, LSC_STAGE_SELECT, LSC_STAGE_READY
} lsc_stage_t;

typedef struct {
    lsc_stage_t stage;
    isp_status_t status;
    uint8_t profile;
    uint32_t expected_count;
    uint32_t load_status;
    uint32_t profile_status;
    uint32_t errors;
} lsc_result_t;

/*
 * Nạp một profile: BEGIN, ghi đúng 4*nx*ny hệ số rồi VALIDATE. coefs phải đủ lsc_coef_count() phần
 * tử UQ4.18, hệ số 1x = LSC_COEF_UNITY (0x040000), tối đa LSC_COEF_MAX. cfg phải khớp hình học
 * LSC_MESH_NODES đã lập trình. Xóa lỗi trước BEGIN, kiểm tra mọi hệ số trước khi nạp, hủy chuỗi
 * thiếu và chỉ thành công khi VALID(profile) được đặt. Từ chối đích đang hoạt động; chờ quá hạn
 * trả ISP_E_TIMEOUT.
 */
isp_status_t  lsc_load_profile(uint8_t profile,
                               const lsc_mesh_config_t *cfg,
                               const uint32_t *coefs);
/*
 * Như lsc_load_profile(), có timeout của bên gọi và chẩn đoán tùy chọn.
 * wait=NULL dùng số lần thăm dò mặc định; result=NULL bỏ ảnh chụp trạng thái.
 * Lỗi phần cứng trả ISP_E_IO; lỗi tham số trả ISP_E_PARAM; quá hạn trả ISP_E_TIMEOUT.
 * Các hàm không tự in log và không tự chọn sang một profile khác.
 */
isp_status_t  lsc_load_profile_ex(uint8_t profile, const lsc_mesh_config_t *cfg,
                                 const uint32_t *coefs, const isp_wait_t *wait,
                                 lsc_result_t *result);

/* Chọn bộ lưu trú 0..2 và bật/tắt LSC. */
isp_status_t  lsc_set_profile(uint8_t profile, bool enable);

/* Đọc lỗi đã chốt LSC_ERROR; write_one_clear khác 0 thì xóa các bit đã đọc. */
uint32_t      lsc_get_errors(bool write_one_clear);

typedef struct {
    lsc_mesh_config_t mesh;
    bool enable;
    uint8_t profile;
    const uint32_t *coefficients[3]; /* NULL: bỏ nạp slot đó. */
    uint32_t coefficient_count[3];  /* Bằng 0 khi con trỏ NULL, còn lại bằng 4*nx*ny. */
} lsc_init_config_t;

/*
 * Mặc định cung cấp ba bảng lưới 7x5 hệ số 1x, khối tắt.
 * Có thể bỏ bảng bằng coefficients[p]=NULL và coefficient_count[p]=0;
 * khi bật LSC, bảng được chọn tại profile phải có dữ liệu.
 * Init nạp một slot khác slot đang chọn trước, xác nhận rồi chọn tạm slot đó
 * để nạp slot cũ. Không tự tạo bảng cho slot NULL. Nếu chỉ có bảng của slot
 * đang chọn, driver thử BEGIN theo MAS. Model giữ ACTIVE_PROFILE đến SOF có
 * thể vẫn từ chối slot active cũ dù đã đổi selector; hàm trả lỗi rõ, không bỏ
 * bảng lỗi. VP hiện tại cần cấu hình tường minh chỉ dùng slot 1/2 khi cold init,
 * hoặc cần bản model sửa quy tắc active để nạp đủ ba bảng trước frame đầu.
 * Hàm lsc_get_default_config() theo reset ở trên vẫn giữ lưới 0/0.
 */
void lsc_get_default_init_config(lsc_init_config_t *cfg);
isp_status_t lsc_validate_init_config(const lsc_init_config_t *cfg, uint16_t w, uint16_t h);
isp_status_t lsc_initialize(const lsc_init_config_t *cfg, uint16_t w, uint16_t h);
isp_status_t lsc_initialize_ex(const lsc_init_config_t *cfg, uint16_t w, uint16_t h,
                               const isp_wait_t *wait, lsc_result_t *result);

#endif /* FTEL_ISP_LSC_H */
