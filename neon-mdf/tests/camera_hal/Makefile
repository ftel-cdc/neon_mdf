# Host tests of the camera HAL. Needs only gcc and make: no cross toolchain, no hardware.
DRV := ../../components/driver
RT := ../../components/neon_camera_runtime
ARCH := ../../arch/RISCV/Device/FTEL/FX1/Include
CC ?= gcc
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Werror -g -O0 -I$(DRV) -I$(RT) -I$(ARCH) -I.
BUILD ?= build

MIPI_SRC := $(DRV)/mipi/platform/common/neon_mipi.c $(DRV)/mipi/platform/fx1/fx1_mipi_caps.c \
            $(DRV)/mipi/platform/fx1/fx1_mipi_simulation.c
CSI_SRC := $(DRV)/csi/platform/common/neon_csi.c $(DRV)/csi/platform/fx1/fx1_csi_caps.c \
           $(DRV)/csi/platform/fx1/fx1_csi_simulation.c
ISP_SRC := $(DRV)/isp/platform/common/neon_isp.c $(DRV)/isp/platform/fx1/fx1_isp_caps.c $(DRV)/isp/platform/fx1/fx1_isp_profile.c \
            $(DRV)/isp/platform/fx1/fx1_isp_simulation.c
RT_SRC := $(RT)/src/neon_camera_runtime.c $(RT)/src/neon_camera_runtime_init.c $(RT)/3a/neon_camera_3a_reference.c
TESTS := $(BUILD)/test_neon_mipi $(BUILD)/test_neon_csi $(BUILD)/test_neon_isp $(BUILD)/test_neon_camera_runtime

.PHONY: all test fuzz clean
all: $(TESTS)

test: all
	@set -e; for t in $(TESTS); do echo "== $$t"; $$t; done

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/test_neon_mipi: test_neon_mipi.c test_common.h $(MIPI_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_neon_mipi.c $(MIPI_SRC)

$(BUILD)/test_neon_csi: test_neon_csi.c test_common.h $(MIPI_SRC) $(CSI_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_neon_csi.c $(MIPI_SRC) $(CSI_SRC)

$(BUILD)/test_neon_isp: test_neon_isp.c test_common.h $(ISP_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_neon_isp.c $(ISP_SRC)

$(BUILD)/test_neon_camera_runtime: test_neon_camera_runtime.c test_common.h $(MIPI_SRC) $(CSI_SRC) $(ISP_SRC) $(RT_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_neon_camera_runtime.c $(MIPI_SRC) $(CSI_SRC) $(ISP_SRC) $(RT_SRC)

# Random model-based tests, with address and undefined-behaviour sanitizers. Slower than the unit tests.
FUZZ_FLAGS := -std=c11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -I$(DRV) -I$(ARCH)
fuzz: | $(BUILD)
	$(CC) $(FUZZ_FLAGS) -o $(BUILD)/fuzz_neon_isp fuzz_neon_isp.c $(ISP_SRC)
	$(CC) $(FUZZ_FLAGS) -o $(BUILD)/fuzz_neon_csi fuzz_neon_csi.c $(MIPI_SRC) $(CSI_SRC)
	$(BUILD)/fuzz_neon_isp 20000
	$(BUILD)/fuzz_neon_csi

clean:
	rm -rf $(BUILD)
