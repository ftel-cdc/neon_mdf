# Chạy Neon ISP

App: `examples/FX1_ISP_Image`. Dùng thư viện Neon và tuning riêng tại
`profile/profile.csrw`, không dùng profile của VP.

## 1. Chuẩn bị

Thực hiện [bước chuẩn bị terminal](FX1_FREERTOS.md#1-chuẩn-bị-terminal) từ
gốc SDK: Python local, compiler trên PATH và các biến `SDK_ROOT`,
`VP_ROOT`, `RAW_FILE`, `FX1_PYTHON`. Bước này dùng chung cho hai app ảnh.
Build ELF không cần repo VP hoặc ảnh RAW.

Trong workspace `Neon_fx1`, repo VP nằm tại `fx1/`:

```sh
export SDK_ROOT=/home/hieurose/Neon_fx1/neon_mdf
export VP_ROOT="$(cd "$SDK_ROOT/../fx1" && pwd)"
```

Đặt `RAW_FILE` trỏ tới ảnh RAW gốc muốn chạy.

## 2. Sửa profile và build

Sửa `examples/FX1_ISP_Image/profile/profile.csrw` nếu cần, rồi chạy:

```sh
cd "$SDK_ROOT/examples/FX1_ISP_Image"
python3 tools/generate_profile.py
cmake --preset FX1 -DNEON_MDF_PYTHON3="$FX1_PYTHON"
cmake --build --preset FX1
```

Hoặc dùng `make build` để thực hiện cùng ba bước.

- Header tự sinh: `profile/profile.h` — không sửa tay.
- Firmware: `build/FX1/fx1_isp_image.elf`.
- Đổi profile phải sinh header và build lại ELF.

## 3. Chạy ảnh

```sh
"$VP_ROOT/vp_neon/run_vp_neon" \
  "$SDK_ROOT/examples/FX1_ISP_Image/build/FX1/fx1_isp_image.elf" \
  "$RAW_FILE"
```

Hai tham số là **ELF** và **RAW gốc**. Thay đường dẫn RAW để chạy ảnh khác.
RAW phải là **2688×1520 BGGR, uint16 little-endian**, đúng **8.171.520 byte**.
Lệnh trên mặc định nhận RAW10 căn trái. Các định dạng được hỗ trợ:

| `--input-format` | Dữ liệu trong uint16 LE | Chuyển sang RAW12 để nạp ISP |
|---|---|---|
| `raw10-left` (mặc định) | RAW10 ở bit 15..6, bit 5..0 bằng 0 | Dịch phải 4 bit |
| `raw10` | RAW10 ở bit 9..0, giá trị 0..1023 | Dịch trái 2 bit |
| `raw12` | RAW12 ở bit 11..0, giá trị 0..4095 | Giữ nguyên |

Ví dụ khi `RAW_FILE` chứa RAW12 căn phải:

```sh
"$VP_ROOT/vp_neon/run_vp_neon" --input-format raw12 \
  "$SDK_ROOT/examples/FX1_ISP_Image/build/FX1/fx1_isp_image.elf" \
  "$RAW_FILE"
```

Script tự chuyển RAW, chạy VP và xuất PNG; không cần copy ELF hay chuyển ảnh riêng.

## 4. Xem kết quả

Mở `$VP_ROOT/vp_neon/result/output.png`.

Cùng thư mục có `output.nv12`, `y.bin`, `uv.bin`, `input.isp16`, `vp.log`
và `report.json`. Terminal phải báo **PASS**; nếu lỗi, xem `vp.log`.
Kết quả cùng tên được ghi đè mỗi lần chạy.

Mặc định chạy **2 frame**, lấy frame cuối để GTM auto có trạng thái từ frame đầu.
Profile mặc định xuất **1920×1080** và dùng bộ hệ số D65; ảnh khác
có thể cần tuning khác.

## 5. Clean

```sh
cd "$SDK_ROOT/examples/FX1_ISP_Image"
make clean
```

Xóa `build/FX1/` và cache; giữ source, profile và kết quả ảnh. Sau đó build lại
như bước 2.

## Tùy chọn thử nghiệm

Chạy các lệnh configure/build sau tại thư mục example:

```sh
# Một frame, giữ cùng profile
cmake --preset FX1 -B build/FX1-1frame -DFX1_IMAGE_FRAME_COUNT=1
cmake --build build/FX1-1frame
```

Truyền ELF trong thư mục build tương ứng cho `run_vp_neon`. Script tự lấy
frame cuối, hỗ trợ 1..4 frame. `make clean` chỉ xóa build mặc định `build/FX1/`.

BLC được chọn tại `profile/profile.csrw`: `BLC_CTRL 0x1` bật,
`BLC_CTRL 0x0` tắt. Sau khi sửa, chạy lại bước 2 để cập nhật ELF.

BLC dùng scale riêng từng kênh. Profile mặc định chọn group G0 bằng
`BLC_GAIN_SEL 0x100` và đặt scale như sau:

```text
BLC_SCALE_G0_R  0x11112
BLC_SCALE_G0_GR 0x11112
BLC_SCALE_G0_GB 0x11112
BLC_SCALE_G0_B  0x11112
```

## Phạm vi profile và build tự động

CMake tự sinh `build/FX1/generated/profile.h` khi `.csrw` hoặc generator đổi;
đây là header compiler dùng. Header `profile/profile.h` do lệnh Python tạo là
bản xem trước. Khi profile thay đổi, lệnh build tự sinh lại header được biên dịch.

Muốn build profile khác mà giữ profile mặc định:

```sh
cmake --preset FX1 -B build/custom -DFX1_ISP_PROFILE="$SDK_ROOT/examples/FX1_ISP_Image/profile/profile.csrw"
cmake --build build/custom
```

Tạo đủ `.elf`, `.elf.map`, `.elf.bin`; VP chạy ELF. BIN là binary thô,
chưa phải image BootROM/flash đóng gói.

Profile mặc định dùng 65 tên thanh ghi. Generator hỗ trợ
BLC group 0, LSC resident 1/2, EE resident 0, Gamma đủ 256 mẫu, GTM auto và
CH0 NV12. Có thể đổi gain/CCM, các bảng và enable của khối trong schema.
Giữ đúng thứ tự lệnh nạp/confirm. Register lạ, LUT thiếu hoặc các mode chưa
hỗ trợ sẽ bị từ chối. Cấu hình còn phải qua kiểm tra tham số của driver khi chạy.
Driver BLC hỗ trợ G0..G7; generator profile của app ánh xạ G0.
LSC dùng UQ4.18, hệ số tối đa 8x (`0x200000`).
Input app cố định 2688×1520; runner hỗ trợ ba cách đóng gói RAW ở mục 3.
Đổi Bayer trong profile phải tương ứng dữ liệu RAW (runner không tự nhận Bayer).
