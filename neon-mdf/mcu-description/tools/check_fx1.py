"""Kiểm tra FX1.yaml, header sinh ra và SVD với CSR/regmap/BSP đang sử dụng."""

import argparse
from copy import deepcopy
import json
from pathlib import Path
import re
import subprocess
import tempfile
from xml.etree import ElementTree as ET

from backends.riscv import gen_fx1_registers
from import_fx1_isp import BEGIN, END, isp_block
from mcu_desc.loader import DeviceDescriptionError, load_device

ROOT = Path(__file__).resolve().parents[2]


def check(args):
    description = ROOT / "mcu-description/devices/FX1.yaml"
    header = args.regmap or ROOT / "arch/RISCV/Device/FTEL/FX1/Include/fx1_isp/fx1_isp_csr.h"
    device = load_device(description)
    registers = next(p for p in device.peripherals if p.name == "ISP").registers
    if args.isp_sdk is not None:
        expected, _ = isp_block(args.isp_sdk / "docs/FTEL_IP_ISP_CSR_v1.07.xlsx", header)
        actual = description.read_text(encoding="utf-8")
        if BEGIN + actual.split(BEGIN, 1)[1].split(END, 1)[0] + END != expected:
            raise ValueError("Phần ISP trong YAML lệch CSR/regmap; chạy lại import_fx1_isp.py")
    gen_fx1_registers.validate(device)
    include = ROOT / "arch/RISCV/Device/FTEL/FX1/Include"
    source = ['#include "FX1.h"', '#define ISP_BASE FX1_ISP_CSR_BASE',
              '#include "ftel_isp_regs.h"', '#include "dma/fx1_dma_regs.h"']
    checks = 0

    def equal(left, right):
        nonlocal checks
        checks += 1
        source.append(f'_Static_assert(({left}) == ({right}), "{left}");')

    # Biên dịch đối chiếu từng macro độc lập; không chỉ so hai lần cùng bộ dữ liệu Python.
    definitions = subprocess.check_output(
        [args.cc, "-E", "-dM", "-x", "c", "-I", str(include),
         "-I", str(ROOT / "components/ftel_isp/registers"),
         "-I", str(args.bsp_include), "-"],
        input="\n".join(source) + "\n", text=True)
    names = re.findall(r"^#define\s+(FX1_ISP_\w+)\s+", definitions, re.M)
    for name in names:
        if (name.endswith(("_OFFSET", "_RESET", "_ACCESS", "_MASK", "_SHIFT", "_WIDTH"))
                and name != "FX1_ISP_MAX_REGISTER_OFFSET"):
            equal("NEON_" + name, name)
    for h in range(device.cpu.extra["hart_count"]):
        equal(f"NEON_FX1_CLINT_MSIP_HART{h}_OFFSET", f"FX1_CLINT_MSIP_OFF + 4u * {h}u")
        for i, part in enumerate(("LO", "HI")):
            equal(f"NEON_FX1_CLINT_MTIMECMP_HART{h}_{part}_OFFSET", f"FX1_CLINT_MTIMECMP_OFF + 8u * {h}u + {4*i}u")
        equal(f"NEON_FX1_PLIC_ENABLE_HART{h}_M_OFFSET", f"FX1_PLIC_ENABLE_OFF + FX1_PLIC_ENABLE_STRIDE * {h}u")
        equal(f"NEON_FX1_PLIC_THRESHOLD_HART{h}_M_OFFSET", f"FX1_PLIC_CONTEXT_OFF + FX1_PLIC_CONTEXT_STRIDE * {h}u")
        equal(f"NEON_FX1_PLIC_CLAIM_COMPLETE_HART{h}_M_OFFSET", f"NEON_FX1_PLIC_THRESHOLD_HART{h}_M_OFFSET + 4u")
    for i, part in enumerate(("LO", "HI")):
        equal(f"NEON_FX1_CLINT_MTIME_{part}_OFFSET", f"FX1_CLINT_MTIME_OFF + {4*i}u")
    for i in range(1, device.cpu.extra["num_irq"]):
        equal(f"NEON_FX1_PLIC_PRIORITY_{i}_OFFSET", f"FX1_PLIC_PRIORITY_OFF + 4u * {i}u")
    equal("NEON_FX1_PLIC_PENDING_OFFSET", "FX1_PLIC_PENDING_OFF")
    dma = (args.bsp_include / "dma/fx1_dma_regs.h").read_text(encoding="utf-8")
    for name in re.findall(r"^#define FX1_DMA_CH_(\w+)\s+0x", dma, re.M):
        for ch in range(8):
            equal(f"NEON_FX1_SYS_DMA_CH{ch}_{name}_OFFSET", f"FX1_DMA_CH({ch}u, FX1_DMA_CH_{name})")
    globals_ = dma.split("/* ---- global registers", 1)[1].split("/* ---- reset values", 1)[0]
    for name in re.findall(r"^#define FX1_DMA_(\w+)\s+0x", globals_, re.M):
        equal(f"NEON_FX1_SYS_DMA_{name}_OFFSET", f"FX1_DMA_{name}")
    with tempfile.TemporaryDirectory(prefix="neon-fx1-check-") as temp:
        cfile = Path(temp) / "registers.c"
        cfile.write_text("\n".join(source) + "\n", encoding="utf-8")
        subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsyntax-only",
                        "-I", str(include), "-I", str(ROOT / "components/ftel_isp/registers"),
                        "-I", str(args.bsp_include),
                        str(cfile)], check=True)
    svd = ET.parse(ROOT / "mcu-description/generated/FX1.svd").getroot()
    peripherals = {p.findtext("name"): p for p in svd.findall("peripherals/peripheral")}
    svd_count = 0
    for p in device.peripherals:
        if not p.registers:
            if p.name in peripherals:
                raise ValueError("SVD không được tạo register giả cho " + p.name)
            continue
        snode = peripherals[p.name]
        sregs = {r.findtext("name"): r for r in snode.findall("registers/register")}
        expected_regs = {r.name for r in p.registers if r.access != "unknown"}
        if sregs.keys() != expected_regs:
            raise ValueError("Danh sách SVD register lệch YAML: " + p.name)
        svd_count += len(sregs)
        for reg in p.registers:
            if reg.access == "unknown":
                continue
            node = sregs[reg.name]
            if int(node.findtext("addressOffset"), 0) != reg.offset:
                raise ValueError("SVD offset sai: " + reg.name)
            reset = node.findtext("resetValue")
            if (reset is None) != (reg.reset is None) or reset is not None and int(reset, 0) != reg.reset:
                raise ValueError("SVD reset sai: " + reg.name)
            sfields = node.findall("fields/field")
            if len(sfields) != len(reg.fields):
                raise ValueError("SVD mất field: " + reg.name)
            for f, sf in zip(reg.fields, sfields):
                access = f.access or reg.access
                if sf.findtext("name") != f.name or int(sf.findtext("bitOffset")) != f.bit_offset or int(sf.findtext("bitWidth")) != f.bit_width:
                    raise ValueError("SVD field sai: " + reg.name + "." + f.name)
                modified = "oneToClear" if access == "w1c" else (
                    "oneToSet" if access in ("w1s", "w1p", "w1sc") else None)
                if sf.findtext("modifiedWriteValues") != modified:
                    raise ValueError("SVD mất ngữ nghĩa W1*: " + reg.name + "." + f.name)
    # Lỗi metadata phải bị chặn trước khi tạo header, không âm thầm dịch bit sai.
    rejected = 0
    for error in ("overlap", "width", "reset", "access", "alignment"):
        bad = deepcopy(device)
        reg = next(p for p in bad.peripherals if p.name == "ISP").registers[0]
        if error == "overlap": reg.fields[1].bit_offset = reg.fields[0].bit_offset
        elif error == "width": reg.fields[0].bit_width = 33
        elif error == "reset": reg.fields[0].reset ^= 1
        elif error == "access": reg.fields[0].access = "not-an-access"
        else: reg.offset = 1
        try:
            gen_fx1_registers.validate(bad)
        except DeviceDescriptionError:
            rejected += 1
        else:
            raise ValueError("Validator bỏ lọt lỗi " + error)
    return dict(result="PASS", isp_registers=len(registers), isp_fields=sum(len(r.fields) for r in registers),
                peripheral_slots=len(device.peripherals), registers=sum(len(p.registers) for p in device.peripherals),
                irq_sources=len(device.interrupts), c_constants_compared=checks, svd_registers=svd_count,
                invalid_descriptions_rejected=rejected, compiler=args.cc,
                csr_xlsx_checked=args.isp_sdk is not None, regmap=str(header), bsp_include=str(args.bsp_include))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--isp-sdk", type=Path, help="Tùy chọn: đối chiếu thêm CSR XLSX trong repo SDK tham khảo")
    parser.add_argument("--regmap", type=Path, help="Đối chiếu header cụ thể, ví dụ regmap local trong module")
    parser.add_argument("--bsp-include", type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.bsp_include is None:
        args.bsp_include = ROOT / "arch/RISCV/Device/FTEL/FX1/Include"
    report = check(args)
    text = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
    print(text, end="")
