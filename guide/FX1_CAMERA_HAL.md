# FX1 camera HAL: MIPI D-PHY, CSI-2 và ISP

Phạm vi: lớp HAL của `mipi`, `csi` và `isp` cho `neon_camera_runtime`. Ba module cùng một bố cục.
Nguồn: MAS "FTEL MIPI CSI-2 controller" V1.1 và "FTEL IP ISP" v1.0 (repo `Neon-Isp/MAS`).

Trạng thái: API, luật dùng chung và backend mô phỏng (sim) của cả ba module xong, có test chạy trên host.
Backend ghi thanh ghi FX1 **chưa viết cho module nào** (xem mục 9). Phần ISP nằm ở mục 11.

## 1. Kiến trúc

```text
components/driver/mipi/                       (csi/ và isp/ có cùng bố cục)
  neon_mipi.h                                 API công khai, handle do caller cấp. Ngoài platform/ chỉ có header
  CMakeLists.txt
  platform/common/
    neon_mipi.c                               mọi luật: validate, state machine, epoch, ownership. Dùng chung mọi chip
    neon_mipi_hw.h                            hợp đồng với backend: neon_mipi_hw_*, trung lập với chip
  platform/fx1/
    fx1_mipi_caps.c                           giới hạn của chip FX1 (lane, tốc độ...), dùng chung cho mọi backend FX1
    fx1_mipi_simulation.c / .h                backend in-RAM + hook test (CONFIG_NEON_MDF_CAMERA_SIMULATION=y, mặc định)
    (fx1_mipi_regs.c)                         backend ghi thanh ghi, chưa viết
```

Luồng gọi: `neon_camera_runtime` gọi hàm trực tiếp (trả `neon_status_t`) → `neon_mipi.c`/`neon_csi.c` →
`neon_*_hw_*` của backend chip. Tên file csi tương ứng: `neon_csi.c`, `neon_csi_hw.h`, `fx1_csi_caps.c`,
`fx1_csi_simulation.c/.h`. Khác `mipi` một file: csi có thêm header công khai `neon_csi_events.h`
(25 bit sự kiện `NEON_CSI_EVENT_*`), do `neon_csi.h` include nên caller chỉ cần include `neon_csi.h`. Nhờ đó
`neon_csi.h` chỉ còn 4 macro sức chứa struct.

`isp` giống hệt về bố cục: `neon_isp.h`, `neon_isp_events.h` (16 bit sự kiện `NEON_ISP_EVENT_*`, do `neon_isp.h`
include), `platform/common/neon_isp.c` + `neon_isp_hw.h`, `platform/fx1/fx1_isp_caps.c`,
`fx1_isp_profile.c` (thanh ghi nào profile tuning được ghi, mục 7.1) và `fx1_isp_simulation.c/.h`. Điểm khác: `fx1_isp_caps.c` đã đọc macro sinh từ YAML (mục 8).

Mỗi chip chỉ có một backend được link. Luật không nằm trong backend, nên backend ghi thanh ghi chỉ phải đọc/ghi
thanh ghi và test của phần lõi vẫn đúng khi thay backend.

`mipi` và `csi` nằm chung một khối CSR 4 KB tại `0x11040000` và chung một IRQ, nên hai module phối hợp
bằng handle chứ không bằng cách chia thanh ghi.

## 2. Thứ tự bring-up (MAS 9.1)

| Bước | Hàm | Việc |
|---|---|---|
| 1 | `neon_mipi_acquire` | Kiểm tra cấu hình, `dphy_rst_req`, bật PHY, chưa áp `active_lanes` |
| 2 | `neon_mipi_wait_ready` | Đợi clock lane và các data lane vào Stop State, rồi áp `active_lanes`. Hết `timeout_polls` thì trả `NEON_TIMEOUT`, link vẫn `ENABLED` |
| 3 | `neon_csi_configure` | Kiểm tra và ghi PROTOCOL, STREAM, DMA, metadata. Core chưa bật |
| 4 | `neon_csi_start` | `neon_mipi_lock` (yêu cầu link `READY`), nạp cả hai buffer cho phần cứng, bật DMA rồi bật core |
| 5 | `neon_isp_configure`, `neon_isp_start` | ISP_EN trước, rồi nạp buffer, rồi bật DMA (mục 11) |
| 6 | sensor `start_stream` | Luôn là bước cuối |

Dừng theo thứ tự ngược lại: sensor `stop_stream`, `neon_isp_stop`, `neon_csi_stop` (mở khóa link), rồi
`neon_mipi_release` (trả `NEON_LOCKED` nếu còn receiver giữ link).

## 3. Phối hợp mipi và csi

- `neon_mipi_lock/unlock`: `csi_start` khóa link, `release` bị từ chối khi `lock_count != 0`.
- `phy_epoch` tăng sau mỗi lần `neon_mipi_recover`. Receiver đang chạy thấy epoch đổi thì bật sự kiện phần
  mềm `NEON_CSI_EVENT_LINK_RESET` (bit 31) và vẫn giữ cấu hình, vì reset PHY không xóa thanh ghi CSI.
- `config_epoch` chỉ tăng khi `NEON_MIPI_RECOVER_FULL` (`full_reset` khôi phục cả register file). Receiver
  chuyển sang `NEON_CSI_STATE_CONFIG_LOST`: gọi `neon_csi_stop`, `neon_csi_configure`, `neon_csi_start` lại.
- `neon_csi_recover(SOFT)` chỉ chạy khi `clock_hs_active`: `soft_reset` đồng bộ vào `rxbyteclkhs`, clock lane
  đang Stop State thì lệnh không có tác dụng. Khi đó trả `NEON_ERROR`, leo lên `neon_mipi_recover(FULL)`.
  Sau soft reset cả hai buffer thuộc về phần cứng, frame phần mềm đang giữ mất.

## 4. Buffer và frame context

- Hai buffer ping-pong. Sau `acquire_frame` phần mềm sở hữu buffer đó tới `release_frame`.
- Khi cả hai buffer đã đầy, frame thứ ba bị phần cứng bỏ trước (`FRAME_DROPPED`, `frame_drop_count`). Số
  frame (`frame_id_bits` bit) vẫn tăng, nên chỗ nhảy `frame_id` cho thấy frame mất.
- `error_policy`: `RELEASE_WITH_ERROR` giao buffer lỗi với `state == SW_ERROR` và `last_valid_line`.
  `DISCARD_AND_REWIND` ghi lại buffer đó, phần mềm không thấy gì ngoài `FRAME_DROPPED`.
- Thanh ghi FRAME_ID/TIMESTAMP chỉ mô tả frame hoàn tất gần nhất. Muốn context chính xác cho từng buffer
  phải bật `metadata.enable` (record trong DDR). Khi không bật và đã có frame mới hơn, `neon_csi_frame_t`
  có `context_valid == false` và các trường context bằng 0.
- Chọn frame cũ nhất khi cả hai đầy: so `frame_id` theo modulo `2^frame_id_bits` khi có metadata. Không có metadata thì
  HAL giả định DMA luân phiên bắt đầu từ side 0. Giả định đó đúng khi phần mềm trả buffer theo thứ tự đã lấy,
  **cần xác nhận trên VP**.

## 5. Layout đầu ra

`neon_csi_calculate_layout` tính `bytes_per_line`, `line_pitch_bytes`, `frame_bytes` mà không cần phần cứng.
Base và pitch phải chia hết cho `caps.dma_align_bytes` (FX1: 16), để ISP IDMA đọc trực tiếp (ISP MAS bảng 154).
`RAW16` = một halfword 16 bit cho mỗi pixel, **căn phải**. `PACKED` = đóng gói CSI-2 gốc.

## 6. Sự kiện

`neon_csi_get_events` và `neon_isp_get_events` trả các bit sự kiện do HAL đánh số (sticky, W1C). CSI có thêm bit
31 do phần mềm. FX1 hiện đặt bit CSI 0 đến 23 trùng IRQ_STATUS, nhưng backend phải dịch tường minh. Thứ tự bit
thật của `DMA_IRQ_STAT` và `COMMON_IRQ_STATUS` của ISP chưa xác nhận (MAS chỉ nói bit 2 là IDMA_DONE), nên bit
ISP là số logic của HAL. `caps.supported_events` cho biết chip nào có sự kiện nào. Mask chỉ quyết định
`irq_pending`, không ẩn bit trạng thái. `clear_events` ghi đúng các bit đã xử lý.

## 7. Dùng trong runtime

Mô tả camera một lần trong `neon_camera_runtime_config_t`, `neon_camera_runtime_init` dựng và nối phần còn
lại (file `src/neon_camera_runtime_init.c`):

```c
neon_camera_runtime_t rt;                     /* caller cấp: mipi, csi, isp, 3A và pipeline */
neon_camera_runtime_config_t cfg;

neon_camera_runtime_get_default_config(&cfg); /* RAW12, RGGB, clock liên tục, 3A reference */
cfg.sensor = &sensor;  cfg.sensor_ops = ops;  cfg.sensor_mode_id = 1;
cfg.width = 1920;  cfg.height = 1080;
cfg.lane_count = 4;  cfg.lane_rate_bps = 1000000000UL;
cfg.memory_base = 0x80000000UL;               /* một vùng DDR cho mọi buffer */
neon_camera_runtime_memory_bytes(&cfg, &cfg.memory_bytes);   /* hoặc tự cấp vùng lớn hơn */
cfg.isp_profile = isp_bin;  cfg.isp_profile_bytes = isp_bin_size;            /* tùy chọn */
cfg.algorithm_tuning = aaa_bin;  cfg.algorithm_tuning_bytes = aaa_bin_size;  /* tùy chọn */

neon_camera_runtime_init(&rt, &cfg);          /* không đụng phần cứng */
neon_camera_runtime_reference_ops()->start(&rt.pipeline);
...                                           /* vòng 3A: on_statistics(&rt.pipeline, NULL) */
neon_camera_runtime_reference_ops()->stop(&rt.pipeline);
neon_camera_runtime_deinit(&rt);              /* NEON_BUSY nếu còn chạy, không đổi gì */
```

`init` suy ra cấu hình từng module từ mô tả đó, nên các module khớp nhau từ đầu:

| Suy ra | Từ đâu |
|---|---|
| `csi_config`: định dạng, hình học, RAW16 | `width`, `height`, `raw_depth` |
| `line_padding_bytes` | làm pitch chia hết cho độ căn DMA (lớn hơn của csi và isp). Ví dụ 1922 điểm ảnh = 3844 byte, đệm 12 byte thành 3856 |
| `isp_config.input_stride_bytes`, `csi_config.dma.buffer_size_bytes` | đúng pitch và `frame_bytes` mà `neon_csi_calculate_layout` trả về |
| stride Y và UV, số byte từng plane | `width` căn lên độ căn DMA. UV xen kẽ ở nửa bề rộng nên cùng stride với Y |
| địa chỉ mọi buffer | cắt từ vùng `memory_base`: các buffer RAW của csi trước, rồi với mỗi buffer ra của ISP là plane Y rồi plane UV, mỗi mảnh làm tròn lên độ căn. Số buffer lấy từ caps |
| `stats_aec/awb/af` | `caps` của ISP, nên chip thiếu bộ nào thì tắt bộ đó |
| 3A | `neon_camera_3a_init` với profile của caller, hoặc profile mặc định. Có `algorithm_tuning` thì nạp qua `algorithm_ops->load_profile` |
| `pipeline.isp_profile` | `isp_profile` sau khi `neon_isp_check_profile` duyệt. Số lần ghi và số bị bỏ qua nằm ở `rt.isp_profile_info` |

Điểm cần biết:

- `init` chỉ kiểm tra thứ nó cần để suy ra: hình học và định dạng, kích thước buffer, vùng nhớ đủ chỗ (kể cả
  đẩy `memory_base` lên độ căn và không vượt 4 GiB). **Luật của từng module vẫn do module đó kiểm lúc `start`**
  (số lane, ISP cần chiều cao chẵn, burst), và `start` hoàn tác nếu bị từ chối. Test
  `test_init_leaves_hardware_rules_to_start` chứng minh điều này với 3 lane và chiều cao lẻ.
- **RAW10 bị từ chối** (`NEON_NOT_SUPPORTED`). csi ghi RAW16 căn phải, ISP đọc căn trái, nên RAW10 sẽ ra ảnh
  tối đi 4 lần mà không báo lỗi. Điều kiện viết theo `layout.sample_align`, nên khi csi có `ALIGN_LEFT` thì RAW10
  tự dùng được (mục 9.3).
- Sau `init`, các trường của `rt.pipeline` chỉ là dữ liệu. Muốn đặt buffer ở chỗ khác thì sửa
  `csi_config.dma.base_addr[]`, `isp_config.output_*_addr[]` trước khi `start`.
- Vẫn dựng tay được một `neon_camera_pipeline_t` như trước (các test cũ làm vậy). `init` chỉ là đường tắt.

### 7.1 Binary tuning

`init` nhận hai binary, cả hai tùy chọn. Runtime không mở file: caller trỏ tới dữ liệu đã nhúng trong flash hoặc
đã đọc từ phân vùng firmware (`neon-fx/firmware/camera/tuning/`).

| Binary | `init` làm gì | Lúc nào ghi xuống phần cứng |
|---|---|---|
| `isp_profile` | `neon_isp_check_profile` kiểm **từng entry** theo bản đồ thanh ghi của chip. Sai thì trả `NEON_INVALID_PARAM` trước khi đụng bất cứ thứ gì | Mỗi lần `start`, sau khi CSI chạy và trước `neon_isp_configure`. Phải sống suốt thời gian dùng pipeline |
| `algorithm_tuning` | Gọi `algorithm_ops->load_profile` một lần. Thay cho `profile`, không được đưa cả hai. Thuật toán không có loader thì `NEON_NOT_SUPPORTED`, loader từ chối thì `NEON_INVALID_PARAM` | Không (chỉ là dữ liệu của 3A) |

Thứ tự trong `start` có lý do: ISP đang dừng là lúc duy nhất nạp bảng an toàn cho mọi khối (gamma là RAM sống,
nạp qua ranh giới frame sẽ rách ảnh), và `configure` chạy sau nên các field HAL quản (bit bật thống kê trong
`AEC_CTRL`, `AWB_CTRL`, `AF_CTRL`) theo cấu hình chứ không theo profile. Gain WB của profile giữ làm điểm xuất
phát cho tới khi 3A ghi gain mới. Mỗi `start` ghi lại profile, vì không giả định thanh ghi giữ nguyên qua stop.

Định dạng `isp_profile` là định dạng `profile_*.bin` của VP: chuỗi entry 8 byte (offset u32, giá trị u32, little
endian), không header, ghi theo đúng thứ tự file (cần cho chuỗi nạp LSC). Quy tắc kiểm ở
`platform/fx1/fx1_isp_profile.c`:

- **Từ chối cả file**: offset không phải thanh ghi, thanh ghi chỉ đọc, thanh ghi có bit W1C (ghi vào sẽ xóa trạng
  thái, ví dụ `COMMON_IRQ_STATUS`, `AEC_STATUS`).
- **Bỏ qua, HAL thắng**: hình học khung, `COMMON_BAYER`, `COMMON_INPUT_FORMAT`, `COMMON_CTRL`, `COMMON_IRQ_EN`, các
  nhóm `DMA_`, `IDMA_`, `ODMA_`, `FRAME_`, `OFMT_`, `OUTPUT_`, `RESIZER_`, `CH0..2_`, các `*_CONTEXT_ID` và
  `AWB_RESULT_CTRL`.
- **Ghi**: mọi thanh ghi tuning còn lại (BLC, LSC, BPC, WB, DG, demosaic, CCM, gamma, CSC, GTM, NR, TNR, EE, CNF,
  cấu hình thống kê, nhóm `TUNE_`).

Bảng thanh ghi lấy từ X-macro `NEON_FX1_ISP_REGISTER_LIST` mà script sinh (`gen_fx1_registers.py`) giờ xuất
thêm vào `fx1_registers.h`, nên không có bảng nào chép tay. Đã chạy cả 7 file `Neon-Isp/profile_tuning/profile_*.bin`
qua bộ kiểm và nạp vào sim: mỗi file 1128 entry, ghi 1123, bỏ qua 5 (`COMMON_BAYER`, `RESIZER_CTRL` và ba
`*_CONTEXT_ID`). Bảng tốn khoảng 19 KB flash trên RV32, phần lớn là chuỗi tên. Muốn gọn hơn thì script sinh cần
xuất thêm tên khối của mỗi thanh ghi, để quy tắc bỏ qua tính được lúc biên dịch.

`reference_start` kiểm tra `csi->mipi == mipi`, rồi hoàn tác các bước đã xong theo thứ tự ngược nếu một bước
sau thất bại. `on_statistics(pipeline, NULL)` đọc thống kê mới nhất từ ISP. ISP chưa có frame nào hoàn tất
(hoặc kết quả đó đã dùng rồi) thì trả 1 và không đổi gì. Runtime **chưa có vòng chuyển frame**: nhận frame
CSI, `neon_isp_queue_input`, ISP xử lý, `neon_isp_reclaim_input`, trả buffer cho CSI, và lấy NV12 ra. Việc đó
vẫn thuộc caller cho tới khi chốt topology (mục 9). `neon_isp_set_context_id` cũng chưa được runtime gọi, vì
cần vòng frame đó mới biết lúc nào ghi.

## 8. Số liệu của chip nằm ở đâu (để thêm FX2)

Header HAL (`neon_mipi.h`, `neon_csi.h`) không chứa con số của chip. Chúng nằm ở ba nơi:

| Loại | Ví dụ | Nằm ở |
|---|---|---|
| Sức chứa của struct | `NEON_CSI_MAX_BUFFERS`, `NEON_MIPI_MAX_LANES` | Header HAL. Chip cần nhiều hơn thì tăng |
| Giới hạn thật của chip | số buffer, burst tối đa, outstanding, QoS, bề rộng `frame_id`, lane hợp lệ, tốc độ lane, sự kiện có thật | `neon_*_caps_t`, do backend điền (`fx1_*_caps.c`). Core chỉ kiểm tra theo caps |
| Bit và field của thanh ghi | vị trí bit IRQ_STATUS, bề rộng field | `mcu-description/devices/FX1.yaml`, sinh ra `NEON_FX1_<PERIPH>_<REG>_<FIELD>_SHIFT/WIDTH/MASK` |

Thêm một chip mới (ví dụ FX2) gồm: thư mục `platform/fx2/` với `fx2_*_caps.c` và backend ghi thanh ghi (và
backend mô phỏng nếu cần), cộng việc thêm một cặp `FX2 <file>` vào mỗi `CMakeLists.txt`, kể cả cho
`platform/common/neon_*.c`. Header HAL, phần lõi và test của lõi không đổi, vì test đổi `handle.caps` để giả
lập một chip khác (`test_core_follows_caps`).

Phần giới hạn thật thực chất là độ rộng field thanh ghi (burst 8 bit, outstanding 7 bit, `frame_id` 14 bit,
`bytes_per_line` 16 bit) và vị trí bit sự kiện. YAML sinh được các macro này.

- **ISP đã dùng**: `fx1_isp_caps.c` include `fx1_registers.h` (thư mục
  `arch/RISCV/Device/FTEL/FX1/Include`, target CMake `neon_mdf_device_FX1`). `max_burst_beats` lấy từ field
  `MAX_BURST_M1`, `max_wb_gain_q8` từ độ rộng field gain WB, và `_Static_assert` kiểm số buffer vừa với độ rộng các mặt
  nạ `IDMA_BUF_VALID`, `ODMA_BUF_FREE`, `ODMA_BUF_DONE`. Makefile của test thêm `-I` tới thư mục đó.
- **mipi và csi chưa dùng được**: peripheral `MIPI` trong `FX1.yaml` đang là `registers: []`. Sau khi nhập
  workbook thanh ghi (như `import_fx1_isp.py` đã làm cho ISP), `fx1_mipi_caps.c` và `fx1_csi_caps.c` nên đọc các
  macro `..._WIDTH` thay cho số viết tay.

Số lượng buffer, kênh ảo, slot trích xuất, bội số lane, độ căn DMA, số bin histogram, giới hạn kích thước
khung và cỡ record metadata không phải thanh ghi nên YAML không khai báo được, chúng ở lại trong `fx1_*_caps.c`.

## 9. Chưa làm và câu hỏi mở

1. **Workbook thanh ghi** `FTEL_MEMORY_MAP_REGISTERS_v1_4.xlsx` (hoặc `fx1_csi2_regs.h` của VP). Thiếu nên
   chưa có bitfield của mipi/csi, và chương DMA, base/size của metadata trong MAS còn "preliminary". Backend
   ghi thanh ghi của mipi và csi chưa thể viết. Với ISP đã có YAML nhưng cũng chưa có backend (mục 11).
   `CONFIG_NEON_MDF_CAMERA_SIMULATION=n` dừng build có chủ đích với thông báo này.
2. **Vòng chuyển frame trong runtime**: MAS ISP là memory-to-memory (CSI DMA → DDR → ISP IDMA → ODMA → NV12),
   truyền trực tiếp không nối được. Cần một vòng: nhận frame CSI → `neon_isp_queue_input` → ISP xử lý →
   `neon_isp_reclaim_input` → trả buffer cho CSI, cùng việc lấy NV12 và ghi `context_id`. Cần chốt topology
   trước khi viết, nên chưa làm.
3. **RAW10**: CSI `RAW16` căn phải, ISP đọc căn MSB trong 12 bit (`{raw[9:0], 2'b00}`). RAW12 khớp, RAW10 lệch
   2 bit. Cần chốt bên nào dịch (cờ trong ISP, hoặc `NEON_CSI_ALIGN_LEFT` mà MAS V1.1 chưa có).
4. Sim mô phỏng theo MAS, không phải silicon. Các hành vi sau cần xác nhận trên VP: thứ tự side của CSI sau
   reset, `BUFFER_OWNERSHIP` sau `soft_reset`, việc chọn side khi một side còn thuộc phần mềm, và con trỏ vòng
   buffer của ISP có về slot 0 sau stop hay soft reset không (HAL đang giả định có).
5. **Bit sự kiện ISP**: thứ tự bit thật của `DMA_IRQ_STAT` và `COMMON_IRQ_STATUS` phải lấy từ workbook CSR.
6. **Giả định khi suy ra `mean_*` của ISP**: `awb.count` là số ô đồng vị trí, và mẫu AEC rộng 12 bit (chia 16).
   Cần so với silicon vì vòng 3A đang chạy trên các giá trị này.
7. `dma_align_bytes` của ISP (16 byte) và của CSI trùng nhau vì cùng độ rộng AXI. Hiện mỗi module khai báo
   riêng trong `fx1_*_caps.c`.
8. **Tuning**: binary profile ISP và tuning 3A đã nạp được qua `init` (mục 7.1). Còn mở:
   - **Định dạng binary**: profile ISP dùng đúng định dạng `profile.bin` của VP, không có header, phiên bản hay
     checksum. `Neon-Isp/DECLARE_ISP_TUNNING.md` cảnh báo `tuning.bin` của firmware thật có thể khác định dạng, và
     README của `neon-fx/firmware/camera` yêu cầu module, sensor mode, ISP revision và checksum. Nên bọc thêm một
     header khi định dạng đó được chốt. Bộ kiểm theo bản đồ thanh ghi đã chặn phần lớn file sai.
   - **Profile viết cho khung khác**: profile mẫu viết cho 2688×1520 BGGR. HAL thắng ở hình học và kiểu Bayer,
     nhưng lưới LSC, vùng AEC (`AEC_ZONE_SIZE`) thì theo profile. Dùng profile sai kích thước sẽ cho ảnh sai mà
     không báo lỗi.
   - **Chọn profile theo nguồn sáng** (A, CWF, D50, D65, D75, H, TL84) khi đang chạy chưa có: MAS 9.5.3 cho phép
     đổi profile giữa hai SOF hoặc giữ input DMA, nhưng gamma không an toàn khi chạy. Hiện chỉ nạp lúc dừng.
   - **3A thật**: `3A_tuning_algorithm/{aec,awb,af}.json` là JSON, chưa có binary và chưa có loader. Thuật toán
     dùng thống kê **theo zone** (lưới 16×16, histogram green 64 bin, 16 ô AF), mà ISP HAL chỉ trả tổng toàn
     khung (phần cứng có lưới tới 32×24, MAS 6.21). Loader của 3A reference nhận đúng một struct
     `neon_camera_3a_profile_t`, nên binary của nó phụ thuộc layout struct của trình biên dịch.
9. Chưa build bằng cmake (máy phát triển không có cmake). Các `CMakeLists.txt` và `Kconfig` mới chỉ kiểm
   cú pháp Kconfig và biên dịch bằng `make` trong `tests/camera_hal`.

## 10. Chạy test

```sh
make -C neon-mdf/tests/camera_hal test        # cần gcc và make, không cần cross toolchain
make -C neon-mdf/tests/camera_hal test BUILD=/tmp/camhal   # build ra thư mục khác
make -C neon-mdf/tests/camera_hal fuzz        # fuzz theo mô hình, có ASan/UBSan, chậm hơn
```

Gồm `test_neon_mipi` (86 kiểm tra), `test_neon_csi` (455), `test_neon_isp` (597) và `test_neon_camera_runtime`
(390; sensor giả, mipi, csi và isp thật trên backend sim). Phần `init` của runtime chạy cả chuỗi bằng 3A
reference thật và đối chiếu các giá trị suy ra với cấu hình viết tay. Biên dịch với
`-Wall -Wextra -Wpedantic -Wconversion -Werror`, và đã chạy sạch dưới `-fsanitize=address,undefined`.

## 11. ISP HAL

### 11.1 Mô hình

Memory-to-memory theo ISP MAS: IDMA đọc frame Bayer từ DDR, mỗi mẫu 16 bit và căn MSB trong 12 bit
(RAW10 = `{raw[9:0], 2'b00}`). ODMA ghi NV12 (plane Y và plane UV xen kẽ, mỗi plane có base và stride riêng).
ISP không có giao diện sensor hay CSI-2. Nó chỉ chung DDR với `csi`, nên caller chuyển buffer từ bên này sang
bên kia (mục 7 và 9).

Cả hai DMA quay vòng N buffer theo thứ tự cố định. Quyền sở hữu nằm trong ba mặt nạ `IDMA_BUF_VALID`,
`ODMA_BUF_FREE`, `ODMA_BUF_DONE`. Vì vậy HAL không nhận chỉ số buffer từ caller: `queue_input` đưa frame vào slot
kế tiếp của vòng, `acquire_output` trả frame hoàn tất cũ nhất.

### 11.2 Luồng dùng

```c
neon_isp_init(&isp);
neon_isp_get_default_config(&cfg);        /* RAW12, RGGB, burst 16, cả ba bộ thống kê bật */
/* điền width, height, input_stride_bytes, y/uv_stride_bytes, output_y_addr[], output_uv_addr[], *_bytes */
neon_isp_configure(&isp, &cfg);           /* validate hết, chưa bật pipeline */
neon_isp_start(&isp);                     /* ISP_EN, nạp buffer, rồi bật hai DMA. Sensor start sau */

neon_isp_queue_input(&isp, &in);          /* buffer CSI vừa hoàn tất. Giữ nguyên tới khi reclaim */
neon_isp_reclaim_input(&isp, &back);      /* trả lại buffer cũ nhất đã đọc xong; back.processed */
neon_isp_acquire_output(&isp, &out);      /* NV12 cũ nhất: y/uv addr, stride, tag, timestamp, sof/eof_count */
neon_isp_release_output(&isp, out.buffer_index);   /* xóa DONE rồi mới đặt FREE */

neon_isp_read_statistics(&isp, &st);      /* NEON_BUSY nếu chưa có frame mới */
neon_isp_set_context_id(&isp, tag);       /* tag của exposure và gain vừa ghi cho sensor */
neon_isp_apply_dynamic_controls(&isp, &controls, &effective_frame_id);

neon_isp_check_profile(bin, size, &info);    /* không cần handle: kiểm file tuning */
neon_isp_load_profile(&isp, bin, size, &info);  /* ISP dừng; xong thì phải configure lại */
```

### 11.3 Các quy tắc đáng nhớ

- **Cấu hình chỉ kiểm theo caps**: kích thước 32..3842 × 32..2162 và chẵn, base và stride chia hết 16 byte
  (độ rộng AXI), stride ≥ 2×width cho input và ≥ width cho plane Y/UV, mỗi buffer đủ byte, không tràn 32 bit,
  không chồng nhau (mọi cặp Y/UV). Độ sâu RAW hoặc kiểu Bayer ngoài mask của chip trả `NEON_NOT_SUPPORTED`,
  giá trị vô nghĩa trả `NEON_INVALID_PARAM`. Bộ thống kê mà chip không có phải tắt trong cấu hình.
- **Thứ tự bring-up**: `ISP_EN` trước, rồi nạp buffer, rồi bật DMA (hình học ra chỉ được lái khi pipeline
  bật). Input xếp hàng trước `start` được xử lý ngay khi chạy.
- **stop và recover** bỏ các frame còn xếp hàng và `reclaim_input` trả chúng với `processed == false`. Output
  đang giữ mất hiệu lực. Soft reset giữ cấu hình, xóa quyền sở hữu, frame đang xử lý và thống kê. `stop` lỗi thì
  ISP vẫn ở `RUNNING` để caller thử lại hoặc `recover`. `configure` lại thì quên các input chưa reclaim.
- **Thống kê**: ba bộ AEC (sau BPC, histogram 64 bin, đếm dưới/trên ngưỡng), AWB (sau demosaic, tổng R/G/B) và
  AF. `read_statistics` chỉ trả `NEON_OK` khi có ít nhất một bộ đã công bố frame mà lần gọi trước chưa trả. Các
  trường `mean_luma/red/green/blue` mà 3A hiện dùng được suy ra từ tổng và số đếm trong lõi (mục 9.6).
- **Context id**: `set_context_id` ghi tag, ISP chốt nó tại SOF và trả lại cùng kết quả của frame đó. Nhờ vậy
  vòng 3A biết kết quả đo ứng với exposure/gain nào. `frame_id` bắt đầu từ 1.
- **Control động**: ISP lấy mẫu control tại SOF. `apply_dynamic_controls` trả `effective_frame_id` là frame đầu
  tiên dùng control mới, để caller so với `target_frame_id` của 3A. Hiện chỉ có gain WB (UQ4.8, 12 bit, 0x100 là
  1.0). Gain 0 bị từ chối vì sẽ làm đen kênh đó.

### 11.4 Hiện trạng so với danh sách thiếu trước đây

| # | Thiếu cũ | Hiện tại |
|---|---|---|
| 1 | Mô hình buffer/DMA | Xong: `queue_input`, `reclaim_input`, `acquire_output`, `release_output`, tag và timestamp đi cùng frame |
| 2 | `configure` thiếu stride, căn, chẵn, Bayer | Xong, kiểm theo caps |
| 3 | Statistics đầy đủ, `CONTEXT_ID` | AEC/AWB/AF tổng, `frame_id`, `context_id`. Chưa có thống kê theo zone |
| 4 | Sự kiện | Xong: 16 bit logic, `get/clear/set_event_mask`. Thứ tự bit thật chưa xác nhận |
| 5 | Control động ngoài WB | Mới có WB, kèm `effective_frame_id`. CCM, gamma, LSC, BLC, NR được đặt qua tuning profile (`neon_isp_load_profile`, lúc dừng) |
| 6 | Vòng đời | Xong: `init/deinit/get_caps/get_status/start/stop/recover` |
| 7 | `int` và bảng ops | Xong: `neon_status_t`, hàm gọi trực tiếp, runtime đã đổi theo |
| 8 | Backend mô phỏng thật | Xong: `fx1_isp_simulation.c` (hook: scene, process, lỗi AXI, observer, đếm) |
| 9 | Hai bản ISP | **Chưa làm**: backend ghi thanh ghi của `driver/isp` chưa có. `components/ftel_isp` chạy từng frame ở slot 0 bằng polling, không có vòng DMA quay, nên chỉ làm điểm xuất phát chứ chưa bọc thẳng được |

Hồi quy cần xử lý (chưa sửa): diff chưa commit của `neon-mdf/CMakeLists.txt` và `neon-mdf/Kconfig` bỏ
`add_subdirectory(components/ftel_isp)` và `rsource components/ftel_isp/Kconfig`. `examples/FX1_ISP_Image`
cần target `neon_mdf_ftel_isp` và `CONFIG_NEON_MDF_FTEL_ISP=y` nên sẽ dừng khi configure. `FX1_ISP_Image_FreeRTOS`
và `ISP_Bayer_AE` cũng tham chiếu `ftel_isp` trong CMakeLists. Chưa build để xác nhận vì máy chưa có cmake.

## 12. Rà soát tính đúng đắn (2026-11-16)

Cách làm: đọc lại toàn bộ lõi và backend sim của mipi, csi, isp, runtime, 3A reference và hai sensor mẫu, đối
chiếu MAS. Rồi `gcc -fanalyzer` (sạch), fuzz theo mô hình (`make fuzz`), và các kịch bản ghép runtime + 3A +
sensor thật. Mọi lỗi dưới đây đều tái hiện được bằng chương trình chạy thật trước khi sửa.

Fuzz: ISP 20.000 chuỗi ngẫu nhiên × 400 thao tác, đối chiếu với mô hình FIFO độc lập (thứ tự thu hồi input, cờ
`processed`, thứ tự output, không giao một buffer hai lần), có cả stop, recover, configure xen giữa. CSI chạy
4 tổ hợp metadata bật/tắt × trả buffer theo thứ tự hoặc lộn xộn, qua chỗ quay vòng `frame_id` 14 bit, có soft
recover và restart.

**Đã sửa:**

| Lỗi | Hậu quả | Sửa |
|---|---|---|
| Runtime chạy lại 3A khi chỉ bộ AF (hoặc AWB) có frame mới mà AEC vẫn cũ | Một kết quả đo bị dùng hai lần: exposure bị chỉnh hai lần (404 → 408 từ cùng một frame). Sim cũ công bố cả ba bộ cùng lúc nên không lộ, phần cứng thì công bố riêng | `on_statistics` chạy một lần cho mỗi `frame_id`, nhớ ở `pipeline.last_statistics_frame_id` (đặt lại khi `start`). Sim có thêm `fx1_isp_sim_hold_statistics` để mô phỏng |
| 3A reference trả lỗi khi `mean_luma` hoặc một kênh màu bằng 0 | Cảnh tối (che ống kính, frame đầu) làm vòng 3A kẹt lỗi mãi, exposure không bao giờ tăng | Trung bình 0 là phép đo hợp lệ: exposure tăng. Bỏ qua AE chỉ khi `aec.valid` sai. Kênh màu bằng 0 thì giữ gain WB đang dùng |
| `frame_id_bits = 32` dịch bit bằng độ rộng kiểu | Hành vi không xác định (FX1 dùng 14 bit nên chưa xảy ra) | Lõi và sim csi trả `UINT32_MAX` khi đủ 32 bit |
| `init` lấy độ căn lớn hơn của csi và isp | Sai nếu một độ căn không phải lũy thừa của 2 (FX1 cùng 16 nên chưa xảy ra) | Lấy bội chung nhỏ nhất |
| Tài liệu `effective_frame_id` và `frame_id` của thống kê | Hứa chính xác hơn thực tế | Ghi rõ `effective_frame_id` là cận trên, và `frame_id` có thể lặp lại khi chỉ bộ khác mới |

**Chưa sửa, cần quyết định:**

1. **Giới hạn exposure của sensor và profile 3A không khớp.** Profile mặc định cho 4–4000 dòng, OS04C10 chỉ nhận
   8–4500. Cảnh sáng: tới frame 11, 3A ra lệnh 4 dòng, sensor từ chối, `on_statistics` trả -1 mãi. IMX675
   (4–4000) không bị. Cách sửa đúng là sensor HAL báo giới hạn của nó (thêm một hàm vào
   `neon_camera_sensor_hal_ops_t`) và runtime chép vào profile 3A, tức là đổi API sensor HAL.
2. **CSI không metadata, trả buffer lộn thứ tự** thì `acquire_frame` trả frame mới trước frame cũ. Đã ghi là giả
   định trong header, nhưng API không chặn. Có thể bắt `release_frame` từ chối khi trả không theo thứ tự lấy
   (lúc metadata tắt), hoặc giữ như hiện tại.
3. Nhỏ, chưa ảnh hưởng FX1: nhánh TX của `neon_mipi_acquire` bỏ qua kiểm tra số lane khi chip hỗ trợ TX (FX1
   không có TX). Sim csi lưu cả bit phần mềm `LINK_RESET` vào event mask của backend; backend thanh ghi phải lọc.
   `stop` của runtime trả -1 ở lần gọi lại sau một lần dừng dở dang, kể cả khi lần này ISP đã dừng được.
4. Ngoài phạm vi code: `target_frame_id` của 3A tính theo bộ đếm frame của ISP nhưng được đưa cho sensor, mà
   sensor đếm frame riêng. Hai sensor mẫu đang bỏ qua tham số này. Thanh ghi exposure và gain trong
   hai sensor mẫu ghi rõ là "review-only", chưa đối chiếu datasheet.

