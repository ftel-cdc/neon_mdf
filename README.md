# neon-mdf

FU2 uses a single `FU2` target/preset with the RTL BootROM v5 layout. Application
vectors are at `0x00020080`; builds produce `<app>_image.bin` containing the
128-byte header to load at `0x00020000`. See [FU2 build and boot contract](guide/FU2.md).

An ESP-IDF-style SDK for embedded microcontrollers: **one MCU description
(YAML) generates the device headers, linker scripts and startup code**.
Architecture-independent drivers share an API across chip families. The SDK also
provides bootloader and OTA integration. Each application includes one SDK
CMake file and remains a separate, independent CMake project.

Both **Arm Cortex-M** (CMSIS) and **RISC-V** (custom CSR/startup support without
CMSIS) use the same code generation pipeline.

**Set up the environment first:** see `SETUP.md` for Windows, macOS and Linux.

FX1 Ethernet: [driver API and ownership](neon-mdf/components/driver/eth/README.md) and
[loopback example](examples/ETH_Loopback/README.md), validated on the VP dev Ethernet model.

Coding conventions: [NEON Coding Convention V1.3](guide/NEON_Coding_Convention_V1.3.md)
and [how they apply to Neon MDF](guide/NEON_MDF_Source_Format_Guideline.md).

## Available targets

| Target | Vendor | Core | Flash | RAM | Bootloader/OTA | Status |
|---|---|---|---|---|---|---|
| `FU2` | FPT Telecom | Cortex-M0, up to 60MHz | 512KB | 128KB | RTL BootROM v5 | Register map v8; build/link and MMIO emulation checked; FPGA timing requires hardware measurements |
| `WEACT_F412` | STMicroelectronics (STM32F412RET6) | Cortex-M4F, 100MHz | 512KB | 256KB | Yes | **Hardware-verified**; the main hardware test board |
| `CH32V003F4P6` | WCH | RISC-V QingKe V2A (RV32EC), 48MHz | 16KB | 2KB | Application-dependent; see below | Build/link verified; **not yet run on hardware** |
| `FX1` / `FX1_VP` | FTEL | RV32IMAFC, ilp32f | ELF in DDR | Local BSP layout | Not ported for FX1 | Bare-metal image and FreeRTOS smoke/ISP checks on VP |

## Directory structure

```text
neon_mdf/
  neon-mdf/                      # Independent SDK; contains no applications
    export.sh                    # Sets NEON_MDF_PATH, like ESP-IDF export.sh
    CMakeLists.txt               # Builds neon_mdf_driver and neon_mdf_device_<TARGET>
    Kconfig                      # Sources component and application Kconfig files

    mcu-description/             # Source of truth for generated headers/linker/startup
      schema/                    # Common and architecture-specific YAML schemas
      devices/<TARGET>.yaml      # MCU memory map, peripherals, IRQs and bootloader layout
      tools/                     # Python loader, model and cortex_m/riscv backends
      generated/                 # Debugger SVD and generated CMake fragments

    arch/
      CortexM/
        CMSIS/CMSIS_5/           # ARM CMSIS-Core vendor files; Core/Include only
        CMSIS/Device/<FAMILY>/   # Generated files and handwritten system_<TARGET>.c
      RISCV/
        Core/                    # CSR helpers, software arithmetic and minimal libc shim
        Device/<VENDOR>/<FAMILY>/ # Generated files and handwritten system_<TARGET>.c
        Port/                    # PFIC interrupt and critical-section support

    components/
      console/                   # FU2 printf/_write over UART; enabled with the UART driver
      driver/                    # Family-specific HAL backends
      bootloader/                # Shared second-stage loader with architecture/family glue
      ota/                       # OTA metadata shared by bootloader and application

    rtos/FreeRTOS-Kernel/        # Upstream FreeRTOS with two RV32E compatibility patches
    cmake/                      # Toolchains, architecture flags, targets and build integration
    tools/
      cmake/project.cmake       # The single SDK CMake file applications include
      templates/                # Bare-metal application template
      templates_freertos/       # FreeRTOS application template
      neon_mdf.py               # CLI for build/flash/monitor/menuconfig
      gen_flashmap.py, build_*.py # Partition generation and firmware image packaging

  examples/
    Blinky/                     # GPIO toggle; bootloader/OTA on WEACT_F412/CH32V003F4P6
    FreeRTOS_Blinky/             # Two FreeRTOS tasks toggling two pins; three targets
    OTA_Demo/                    # Factory, OTA and deliberately invalid OTA variants
    Printf_Log/                  # FU2 word-only printf through _write/UART0
    UART_ECHO_Polling/           # FU2 UART0 polling echo; configurable baud/clock/timeout
    GPIO_Input_Output/           # FU2 PB5 button controls PB1 LED; logs over UART0
    FPGA_Terminal/               # FU2 UART0 terminal and testcase directory

  vscode-extension/             # Build/flash/monitor/debug/menuconfig UI; see VSCODE.md
```

Each `examples/<App>/` directory has its own `CMakeLists.txt`,
`CMakePresets.json` and `.vscode/` configuration. Applications configure and
build independently of each other and of the SDK, just like ESP-IDF examples.

FU2 Flash Tool **1.0** provides the `flash_uart_fu2` CMake target. Port and baud
settings are supplied at execution time. Select boot mode 100 and press RESET
when the tool is ready. See [FU2 UART programming](guide/FU2Flashing.md) and
[FU2 boot modes](guide/FU2BootModes.md) for image preparation and boot procedures.

See `guide/ArchitectDetail.md` for the dependency graph between CMake targets.

## Try an example

For a new application, see `guide/FirstApp.md`. To build an existing example:

```sh
. neon-mdf/export.sh              # Once per terminal session
cd examples/Blinky
cmake --preset WEACT_F412
cmake --build --preset WEACT_F412
# -> build/WEACT_F412/{bootloader,blinky,blinky_ota}.{elf,bin,map}
```

Use `FU2` or `CH32V003F4P6` instead to select another target. The same
`Src/main.c` uses the appropriate family driver selected by CMake.

## Bootloader and OTA

`WEACT_F412`, `STM32F103CB` and `CH32V003F4P6` declare `bootloader:` in their
device YAML. Their applications also build a `bootloader` executable with
SDK-provided `main.c`. Programming uses SWD/JTAG or the custom UART protocol:
SYNC, a 16-byte header (magic/size/crc32), 256-byte chunks and CRC32 verification.

An application with `flashmap.csv` defines `app`, `otadata` and `app_ota`
partitions, similar to ESP-IDF `partitions.csv`. OTA uses an **asymmetric**
layout: updates never overwrite the factory `app` slot; they write only
`app_ota`. The `otadata` partition stores `active_slot`, `state` and
`boot_attempts`.

The application calls `neon_ota_confirm_valid()` after running reliably. An
unconfirmed image that repeatedly resets beyond `NEON_OTA_MAX_BOOT_ATTEMPTS`
(default 3) **automatically rolls back to the factory image**. Factory boot,
OTA switching and rollback have been verified on WEACT_F412 hardware.

See `guide/BootloaderOTA.md` for partition definitions, CMake integration, API
calls and factory-to-OTA-to-rollback testing.

Bare-metal `examples/Blinky` on `CH32V003F4P6` builds three executables:
`bootloader`, `blinky` and `blinky_ota`. Its
`flashmap.CH32V003F4P6.csv` fits two application images into the 12KB remaining
after the 4KB bootloader.

`examples/FreeRTOS_Blinky` on the same chip has no OTA partition map. The
minimal FreeRTOS build already uses about 8.5KB Flash and 2KB RAM
(measured on 2026-08-11: text+data=8488B, bss=1936B), leaving insufficient space
for two application images in the chip's 16KB Flash and 2KB RAM. It builds one
application through `neon_mdf_add_app_executable()`. Both configurations use a
bootloader that verifies the header/CRC32 and receives UART updates; the
FreeRTOS version overwrites one fixed slot without A/B switching or rollback.

`FU2` does not declare `bootloader:`. It builds one application and its
BootROM image, without OTA.

## FreeRTOS

FX1 now includes a CLINT/PLIC FreeRTOS port, an ISP worker service and
RAW-image/service/smoke examples. See [FX1 FreeRTOS](guide/FX1_FREERTOS.md).
The existing bare-metal image example and its tuning are retained.

`FreeRTOS_Blinky` supports all three targets using upstream FreeRTOS ports:

- `FU2` (Cortex-M0): `GCC_ARM_CM0`.
- `WEACT_F412` (Cortex-M4F): `GCC_ARM_CM4F`.
- `CH32V003F4P6` (RISC-V without CLINT): `GCC_RISC_V_GENERIC` with the
  `RISCV_no_extensions` chip extension. The application configures the QingKe
  STK tick timer and switches `mtvec` to Direct mode in
  `vPortSetupTimerInterrupt()`; see the opening comments in `Src/main.c`.
  Two scoped patches in `portable/GCC/RISC-V/` support RV32E's reduced
  16-register file.

See `guide/FreeRTOSPort.md` for port selection, Direct Routing and CLINT-less
integration on a new CPU architecture.

## FU2 printf logging

Enable `CONFIG_NEON_MDF_DRIVER_UART=y` for both UART and `printf/_write`;
there is no separate printf flag. Disabling UART excludes the driver and
console.

Include `<stdio.h>` and call
`printf("counter=%u hex=0x%08lX\n", 1U, 0x1234UL)`.
The first `_write` initializes UART0 at 19200 baud, 8N1.
`neon_console_attach_uart()` optionally selects a different UART or baud rate.

The word-only formatter supports strings, integers and hexadecimal output,
without floating-point formatting or full stdio support. STM32F103 and
WEACT_F412 have no built-in UART printf retarget; use their UART APIs directly
or provide an application console.

See [Printf_Log](examples/Printf_Log/README.md) and
[PrintfLogging.md](guide/PrintfLogging.md) for API details, wiring, monitoring
and verification.

## menuconfig (Kconfig)

Each application's `sdkconfig` selects which drivers are compiled. See
`guide/MenuConfig.md` to add options.

```sh
cmake --build --preset WEACT_F412 --target menuconfig   # Like idf.py menuconfig
cmake --preset WEACT_F412                              # Reconfigure to apply changes
```

## VS Code

Open an application directory such as `examples/Blinky/`. Install
`neon-mdf-tools` from `vscode-extension/`. The status bar selects the target,
transport and serial port. Commands provide Build, Flash, Debug, Serial Monitor
and menuconfig; the Create New App wizard offers bare-metal and FreeRTOS
templates.

See `VSCODE.md` for usage and `guide/Debugging.md` for the flash-then-attach
workflow and two hardware issues that have been diagnosed and fixed.

## CLI without VS Code

```sh
. neon-mdf/export.sh
cd examples/Blinky
python3 $NEON_MDF_PATH/tools/neon_mdf.py build flash-jtag monitor
```

Actions include `build`, `flash-jtag`, `flash-uart-stm32`, `flash-ota`,
`monitor`, `menuconfig` and `set-target`. Multiple actions run sequentially
in one command.

## Add an MCU target

See `guide/AddNewMCU.md` for the full procedure:

1. Write `neon-mdf/mcu-description/devices/<TARGET>.yaml` using the common
   schema and `schema/arch/<arch>.schema.yaml`. Use verified addresses and
   register layouts; mark unknown data as PLACEHOLDER. See `guide/DeviceYAML.md`.
2. Register `NEON_MDF_TARGET_<TARGET>_{ARCH,FAMILY,...}` in
   `neon-mdf/cmake/mcu_variants.cmake`.
3. Write `system_<TARGET>.c/.h` for the chip's actual clock setup.
4. For a new family, add a GPIO backend in `components/driver/gpio/`. A
   bootloader also requires Flash and UART backends in
   `components/driver/flash/` and `components/bootloader/`.
   See `guide/DriverImplement.md`.
5. Add a preset to `neon-mdf/cmake/presets/targets.json`, shared by applications
   through `include`.

## Detailed guides

| File | Contents |
|---|---|
| `guide/FirstApp.md` | Create an application, understand its files, build, flash, view logs and edit code. |
| `guide/AddNewMCU.md` | Add a target: YAML, CMake registration, drivers, optional bootloader/OTA and FreeRTOS, and verification. |
| `guide/DeviceYAML.md` | Complete reference for device YAML fields. |
| `guide/DriverImplement.md` | Shared APIs, family backends and required, conditional or Kconfig-controlled CMake integration. |
| `guide/MenuConfig.md` | Define bool/int/hex/string options and use them in C and CMake. |
| `guide/PrintfLogging.md` | FU2 UART printf configuration, formats, monitoring and word-only verification. |
| `guide/ArchitectDetail.md` | CMake target dependencies and include dependencies between layers. |
| `guide/BootloaderOTA.md` | Add OTA partitions, CMake integration and API calls; test factory/OTA/rollback. |
| `guide/FreeRTOSPort.md` | Select ports and integrate Direct Routing or CLINT-less operation. |
| `guide/Debugging.md` | VS Code debugging and hardware fixes for header erasure by `load` and unreliable reset. |

`ARCHITECTURE.md` explains the design rationale; `guide/` provides procedures.

## Current limitations

- `FU2` uses the Common Specification v8 register map and RTL BootROM v5 image
  contract. MMIO emulation does not replace timing measurements and hardware
  verification on FPGA. See `guide/FU2.md` and `FU2_PORTING_STATUS.md`.
- `CH32V003F4P6` drivers, bootloader and FreeRTOS port are build/link verified
  and await hardware testing; the ordered board has not arrived. Extension and
  CLI flashing integration also awaits `wlink`, which differs from
  Cortex-M OpenOCD/SWD programming.
- VS Code debugging currently supports Cortex-M through OpenOCD/SWD. RISC-V
  requires WCH's OpenOCD fork and has not been validated.
- Polling driver timeouts count polling iterations, rather than milliseconds.
  FU2 SRAM/DMA accesses use aligned 32-bit words; byte buffers must own each
  complete word that they occupy.

## FX1 ISP

The `FX1` preset builds firmware with local startup/linker/BSP; VP is only
required to run the ELF. Start with [FX1 Quickstart](guide/FX1_Quickstart.md).

- Bare-metal image: [FX1_ISP_Image](examples/FX1_ISP_Image/README.md) and
  [image run guide](guide/FX1_NEON_ISP_RUN.md).
- FreeRTOS: [FX1 FreeRTOS](guide/FX1_FREERTOS.md), covering the port,
  ISP worker service, init/AEC checks, smoke and the RAW-image example.
