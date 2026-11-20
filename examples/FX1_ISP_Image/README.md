# FX1_ISP_Image — Neon ISP với profile ứng dụng

App dùng `neon_mdf_ftel_isp`, nạp full tuning D65 và chạy 2 frame bằng polling.
RAW đầu vào 2688×1520 BGGR RAW12; output NV12 1920×1080, stride 2688.
`sdkconfig` mặc định bật thư viện 3A kín: loader và thuật toán lấy từ `.a`,
hai binary camera được nhúng vào ELF. Không dùng FreeRTOS.

[Hướng dẫn đầy đủ: build, chuyển RAW, chạy VP và xuất PNG](../../guide/FX1_NEON_ISP_RUN.md).

Từ thư mục example:

```sh
python3 tools/generate_profile.py
cmake --preset FX1
cmake --build --preset FX1
```

ELF nằm ở `build/FX1/fx1_isp_image.elf`. Tuning nằm tại
`profile/profile.csrw`; generator tạo `profile/profile.h` để điền
`isp_config_t`. Không biên dịch Reference driver, không dùng SEAL/APPLY.

Chạy một frame, giữ cùng tuning:

```sh
cmake --preset FX1 -B build/FX1-1frame -DFX1_IMAGE_FRAME_COUNT=1
cmake --build build/FX1-1frame
```

Bật/tắt BLC bằng `BLC_CTRL 0x1` / `BLC_CTRL 0x0` trong
`profile/profile.csrw`, rồi sinh lại header và build. Không dùng cờ CMake. Số frame mặc định 2, có thể chọn 1..4; phải nạp RAW cho từng
slot được chạy. Thư viện quản lý các slot không chồng bộ nhớ.

Full tuning hai frame đã kiểm chứng NV12 khớp từng byte với ProfileReplay và
PNG khớp toàn bộ pixel với `image/elf_all_on_output.png`. Hash NV12:
`4822cf3199fef425980e47a2c357e97745517f3f39ef50066c248224ba912627`.
Một frame có hash khác do trạng thái GTM auto ban đầu. Xem hướng dẫn để chọn
đúng địa chỉ load/dump theo số frame.

Hash trên thuộc đường profile tĩnh khi **tắt 3A**. Khi bật 3A, startup WB từ
binary thay WB của profile nên ảnh không còn dùng hash chuẩn đó.

## Tích hợp binary 3A

Các file bàn giao đặt tại:

```text
neon-mdf/components/ftel_isp/algorithms/isp_3a/
  include/fpt_3a.h
  lib/fx1/libisp_3a.a
examples/FX1_ISP_Image/profile/
  fpt.tuned.sony_imx675.bin
  fpt.sensormodule.sony_imx675.bin
```

Thư viện là bản `isp_3a_distribution` của SDK, chứa cả parser. Không thay bằng
archive thuật toán-only ở gốc thư mục build SDK. Neon build không cần source
hay đường dẫn tới `isp_19_11`. Header không lộ state thuật toán.

`CONFIG_NEON_MDF_FTEL_ISP_3A` nằm trong Kconfig ISP hiện có; dùng menuconfig
để tắt nếu cần đường profile tĩnh. Hai `.bin` chọn qua CMake
`FX1_3A_SENSOR_BIN` và `FX1_3A_TUNING_BIN`; build nhúng nguyên bytes bằng
assembly `.incbin`, giữ chúng ở vùng read-only trong ELF. Không sửa header
profile hay file trong `build/.../generated/` bằng tay.

Chế độ mặc định `FX1_3A_REPLAY=ON` vì binary IMX675 mô tả 2592×1944 RGGB
RAW10, khác RAW của example. AE tạo đề xuất tham chiếu, không áp exposure/gain
lên ảnh RAW đã nạp. Các giá trị `3A_EFFECTIVE_*` giữ metadata baseline của
replay; không phải controls đã được xác nhận trên sensor thật.

AWB ROI screening cần số mẫu scalar UE/OE thật. CSR v1.07 hiện không cung cấp
đủ các số này, nên mặc định thư viện giữ startup WB và đặt cờ AWB_HELD.
Muốn thử đường hội tụ WB trên replay, bật tường minh:

```sh
cmake --preset FX1 -B build/3a-unscreened \
  -DFX1_3A_REPLAY_UNSCREENED_AWB=ON -DFX1_IMAGE_FRAME_COUNT=4
cmake --build build/3a-unscreened
```

Đường này bỏ ROI screening trong bản tuning RAM, vẫn giữ ngưỡng qualification
RGB phần cứng. Log và result luôn đánh dấu `UNSCREENED_AWB`; không dùng làm
kết quả tuning camera sản phẩm. Sau `wait`, app thu và giải phóng lease stats,
chạy 3A, release output, rồi áp WB và context mới khi ISP/DMA idle trước submit
frame kế. Exposure/gain mới chỉ được log; không giả lập việc sensor đã áp.

Strict mode kiểm tra camera mode; với bộ binary hiện tại lệnh sau build được
nhưng firmware sẽ từ chối init trước khi cấu hình/chạy ISP:

```sh
cmake --preset FX1 -B build/3a-strict -DFX1_3A_REPLAY=OFF
cmake --build build/3a-strict
```

Log: `3A_FRAME_ID`, `3A_CONTEXT_ID`, `3A_FLAGS`, `3A_LUMA`, controls, Lux/CCT
và `3A_RESULT=PASS`. Flags: bit 0 replay, bit 1 AWB held, bit 2 unscreened AWB,
bit 3 Lux valid, bit 4 CCT valid, bit 5 CCT còn là startup seed.
Chỉ đọc Lux/CCT khi bit valid tương ứng bật. App cấp 160 KiB vùng nhớ opaque,
kiểm tra size/alignment yêu cầu của archive tại init; state giữ xuyên các frame.

Test VP với UE/OE tính sẵn dùng một file `CC/awb_ue_oe.json` cho cả 9 ảnh.
Chỉ executable test ghép các số này vào stats; firmware thường và component
SDK không đọc file hay tự đếm UE/OE.

```sh
# Tại thư mục example:
cmake --build --preset FX1 --target fx1_isp_image_vp

# Từ gốc workspace:
python3 fx1/vp_neon/run_vp_neon \
  neon_mdf/examples/FX1_ISP_Image/build/FX1/fx1_isp_image_vp.elf \
  CC/D65_raw_2688x1520_5376.raw --awb-counts CC/awb_ue_oe.json
```

## Dọn build

Từ thư mục `examples/FX1_ISP_Image`:

```sh
make clean
```

Xóa `build/FX1/`, gồm ELF/MAP/BIN và CMake cache. Giữ nguyên
source, tuning, header đã sinh và ảnh/log trong `artifacts/`. Các build folder
thử nghiệm khác không bị xóa. Sau đó chạy lại ba lệnh build ở trên;
`make build` cũng thực hiện cùng ba bước sinh header, configure và build.

## Profile độc lập của ứng dụng

- `profile/profile.csrw`: sửa tuning tại đây.
- `profile/profile.h`: được sinh bởi `tools/generate_profile.py`.

App không đọc CSR profile của VP hoặc ProfileReplay. Profile mặc định giữ
bộ hệ số đã kiểm chứng trên D65. Thay các tham số được API Neon hỗ trợ,
sinh lại header rồi build. Tên profile chung không tự tạo calibration cho
mọi nguồn sáng; hãy chọn tuning phù hợp với ảnh.

Cùng ELF chạy được các RAW khác nhau cùng định dạng BGGR RAW10 căn trái,
2688×1520 bằng `run_vp_neon ELF RAW`. Đổi tuning phải build lại ELF.
Kích thước/Bayer/depth đầu vào vẫn được đặt trong main.c, chưa tự nhận mọi
độ phân giải hoặc định dạng RAW.

## Kiểm tra profile lúc build

CMake tự sinh và dùng `build/FX1/generated/profile.h`; không phụ thuộc việc
nhớ chạy Python trước. Có thể chọn file riêng bằng `-DFX1_ISP_PROFILE=/path/profile.csrw`.
Xem phạm vi schema và các mode bị từ chối trong hướng dẫn. BIN là binary thô;
VP dùng ELF. Các example FX1 khác đã được xóa, chỉ giữ FX1_ISP_Image.
