"""Sinh header/SVD FX1 từ YAML, giữ ngữ nghĩa access của CSR v1.07."""

from pathlib import Path
import re
from xml.etree import ElementTree as ET

from mcu_desc.loader import DeviceDescriptionError

ACCESS = {name: i for i, name in enumerate(
    ("ro", "rw", "w1c", "w1s", "w1p", "w1sc", "mixed", "unknown", "wo"))}
BASE_MACROS = {"CLINT": "CLINT", "PLIC": "PLIC", "UART": "UART", "SYS_DMA": "SYS_DMA_CSR",
               "SIM_CTRL": "SIM_CTRL", "I2C0": "I2C0", "ISP": "ISP_CSR", "NPU": "NPU_CSR",
               "H264_H265": "H264_H265_CSR", "ETH": "ETH_CSR", "MIPI": "MIPI_CSR",
               "DDR3": "DDR3_CSR", "DDR_SYSCTRL": "DDR_SYSCTRL"}


def _require(condition, message):
    if not condition:
        raise DeviceDescriptionError("FX1: " + message)


def _identifier(name):
    _require(re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name), f"tên C không hợp lệ: {name}")


def validate(device):
    """Chặn metadata sai trước khi tạo struct MMIO/mask."""
    names = set()
    for peripheral in device.peripherals:
        _identifier(peripheral.name)
        _require(peripheral.name not in names, "trùng peripheral " + peripheral.name)
        names.add(peripheral.name)
        _require(peripheral.name in BASE_MACROS, "instance chưa hỗ trợ: " + peripheral.name)
        registers = set()
        end = 0
        for reg in peripheral.registers:
            ctx = peripheral.name + "." + reg.name
            _identifier(reg.name)
            _require(reg.name not in registers, "trùng register " + ctx)
            registers.add(reg.name)
            _require(reg.size == 32 and reg.offset >= end and reg.offset % 4 == 0,
                     "register phải 32 bit, căn 4 byte và không chồng lấn: " + ctx)
            limit = {"CLINT": 0x10000, "PLIC": 0x1000000}.get(peripheral.name, 0x10000)
            _require(reg.offset + 4 <= limit, "register vượt aperture: " + ctx)
            end = reg.offset + 4
            _require(reg.access in ACCESS, "access không hợp lệ: " + ctx)
            _require(reg.access != "mixed" or reg.fields, "Mixed cần access từng field: " + ctx)
            _require(reg.reset is None or 0 <= reg.reset <= 0xFFFFFFFF, "reset ngoài 32 bit: " + ctx)
            used = 0
            fields = set()
            for field in reg.fields:
                fctx = ctx + "." + field.name
                _identifier(field.name)
                _require(field.name not in fields, "trùng field " + fctx)
                fields.add(field.name)
                _require(0 <= field.bit_offset < 32 and 1 <= field.bit_width <= 32 - field.bit_offset,
                         "field vượt 32 bit: " + fctx)
                mask = ((1 << field.bit_width) - 1) << field.bit_offset
                _require(field.mask is None or (field.mask > 0 and not field.mask & ~mask),
                         "mask ngoài field: " + fctx)
                _require(not used & mask, "field chồng bit: " + fctx)
                used |= mask
                _require((field.access or reg.access) in ACCESS, "access field sai: " + fctx)
                _require((field.access or reg.access) != "mixed", "Mixed phải có access từng field: " + fctx)
                if field.reset is not None:
                    _require(0 <= field.reset < 1 << field.bit_width, "reset field quá rộng: " + fctx)
                    if reg.reset is not None:
                        _require((reg.reset & mask) >> field.bit_offset == field.reset,
                                 "reset field khác reset register: " + fctx)


def _comment(text):
    return " ".join(text.replace("*/", "* /").split())


def register_header(device):
    lines = ["/* Sinh từ FX1.yaml; không sửa trực tiếp. */",
             "/* Access UNKNOWN/reset không có nguồn: không suy diễn thành RW/0. */",
             "#ifndef NEON_FX1_REGISTERS_H", "#define NEON_FX1_REGISTERS_H",
             "#include <stdint.h>", "#include <stddef.h>",
             '#include "fx1_isp/fx1_isp_csr.h"',
             "#ifdef __cplusplus", "#define NEON_FX1_OFFSET_ASSERT static_assert", "#else",
             "#define NEON_FX1_OFFSET_ASSERT _Static_assert", "#endif"]
    for access, number in ACCESS.items():
        lines.append(f"#define NEON_FX1_ACCESS_{access.upper()} {number}u")
    for p in device.peripherals:
        prefix = "NEON_FX1_" + p.name
        lines += ["", f"/* {_comment(p.description)} */", f"#define {prefix}_BASE 0x{p.base:08X}u"]
        if not p.registers:
            continue
        for reg in p.registers:
            key = prefix + "_" + reg.name
            lines += [f"/* {_comment(reg.description)} */", f"#define {key}_OFFSET 0x{reg.offset:08X}u",
                      f"#define {key}_ACCESS NEON_FX1_ACCESS_{reg.access.upper()}",
                      f"#define {key}_RESET_KNOWN {int(reg.reset is not None)}u"]
            if reg.reset is not None:
                lines.append(f"#define {key}_RESET 0x{reg.reset:08X}u")
            masks = {a: 0 for a in ACCESS if a != "mixed"}
            for field in reg.fields:
                fkey = key + "_" + field.name
                mask = field.mask if field.mask is not None else ((1 << field.bit_width) - 1) << field.bit_offset
                access = field.access or reg.access
                masks[access] |= mask
                lines += [f"/* {_comment(field.description)} */",
                          f"#define {fkey}_SHIFT {field.bit_offset}u",
                          f"#define {fkey}_WIDTH {field.bit_width}u",
                          f"#define {fkey}_MASK 0x{mask:08X}u",
                          f"#define {fkey}_ACCESS NEON_FX1_ACCESS_{access.upper()}"]
                if field.reset is not None:
                    lines.append(f"#define {fkey}_RESET 0x{field.reset:08X}u")
            if not reg.fields and reg.access != "mixed":
                masks[reg.access] = 0xFFFFFFFF
            for access, mask in masks.items():
                lines.append(f"#define {key}_{access.upper()}_MASK 0x{mask:08X}u")
        lines.append("typedef struct {")
        offset, gap = 0, 0
        for reg in p.registers:
            if reg.offset > offset:
                lines.append(f"    uint32_t RESERVED_{gap}[{(reg.offset - offset) // 4}];")
                gap += 1
            qualifier = "volatile const" if reg.access == "ro" else "volatile"
            lines.append(f"    {qualifier} uint32_t {reg.name}; /* 0x{reg.offset:04X}, {reg.access} */")
            offset = reg.offset + 4
        lines.append(f"}} {prefix}_Regs_t;")
        lines.append(f"#define {prefix} (({prefix}_Regs_t *)(uintptr_t){prefix}_BASE)")
        for reg in p.registers:
            lines.append(f'NEON_FX1_OFFSET_ASSERT(offsetof({prefix}_Regs_t, {reg.name}) == '
                         f'{prefix}_{reg.name}_OFFSET, "{p.name}.{reg.name}");')
    lines += ["#undef NEON_FX1_OFFSET_ASSERT", "#endif", ""]
    # Public ISP names resolve to the canonical FX1 BSP definitions.
    isp = next(p for p in device.peripherals if p.name == "ISP")
    aliases = set()
    for reg in isp.registers:
        key = "NEON_FX1_ISP_" + reg.name
        aliases.update(key + "_" + suffix for suffix in ("OFFSET", "RESET"))
        for field in reg.fields:
            aliases.update(key + "_" + field.name + "_" + suffix
                           for suffix in ("SHIFT", "WIDTH", "MASK", "RESET"))
    for i, line in enumerate(lines):
        match = re.fullmatch(r"#define (\w+) (?:0x[0-9A-Fa-f]+|[0-9]+)u", line)
        if match and match[1] in aliases:
            lines[i] = f"#define {match[1]} {match[1][5:]}"
    return "\n".join(lines)


def svd(device):
    """SVD cho debugger; không tạo reset giả cho null, không làm mất access từng field."""
    def tag(parent, name, value):
        ET.SubElement(parent, name).text = str(value)

    def access(parent, value):
        mapping = {"ro": "read-only", "wo": "write-only", "w1p": "write-only",
                   "rw": "read-write", "mixed": "read-write", "w1c": "read-write",
                   "w1s": "read-write", "w1sc": "read-write"}
        if value in mapping:
            tag(parent, "access", mapping[value])
        if value in ("w1c", "w1s", "w1p", "w1sc"):
            tag(parent, "modifiedWriteValues", "oneToClear" if value == "w1c" else "oneToSet")

    root = ET.Element("device", {"schemaVersion": "1.3"})
    tag(root, "vendor", device.vendor)
    tag(root, "name", device.device)
    tag(root, "version", "VP_FX1_V3.0_ISP_1.07")
    tag(root, "description", device.description + " W1P=pulse, W1SC=self-clear; xem CSR trước khi ghi.")
    tag(root, "addressUnitBits", 8)
    tag(root, "width", 32)
    peripherals = ET.SubElement(root, "peripherals")
    for p in device.peripherals:
        if not p.registers:
            continue  # Không hiện IP reserved thành register có thể đọc trong debugger.
        node = ET.SubElement(peripherals, "peripheral")
        tag(node, "name", p.name)
        tag(node, "description", p.description)
        tag(node, "baseAddress", hex(p.base))
        block = ET.SubElement(node, "addressBlock")
        tag(block, "offset", 0)
        tag(block, "size", hex(max(r.offset + 4 for r in p.registers)))
        tag(block, "usage", "registers")
        irq_name = "DDR" if p.name == "DDR3" else p.name
        for irq in device.interrupts:
            if irq.name == irq_name:
                irq_node = ET.SubElement(node, "interrupt")
                tag(irq_node, "name", irq.name)
                tag(irq_node, "value", irq.number)
        registers = ET.SubElement(node, "registers")
        for reg in p.registers:
            if reg.access == "unknown":
                continue  # Không để debugger suy diễn quyền truy cập mặc định.
            rnode = ET.SubElement(registers, "register")
            tag(rnode, "name", reg.name)
            tag(rnode, "description", reg.description + " [access=" + reg.access + "]")
            tag(rnode, "addressOffset", hex(reg.offset))
            tag(rnode, "size", 32)
            access(rnode, reg.access)
            if reg.reset is not None:
                tag(rnode, "resetValue", hex(reg.reset))
            if ((p.name == "PLIC" and reg.name.startswith("CLAIM_COMPLETE")) or
                    (p.name in ("UART", "I2C0") and reg.name in ("DR", "RDATA"))):
                tag(rnode, "readAction", "modify")
            if reg.fields:
                fields = ET.SubElement(rnode, "fields")
                for f in reg.fields:
                    fnode = ET.SubElement(fields, "field")
                    tag(fnode, "name", f.name)
                    tag(fnode, "description", f.description + " [access=" + (f.access or reg.access) + "]")
                    tag(fnode, "bitOffset", f.bit_offset)
                    tag(fnode, "bitWidth", f.bit_width)
                    access(fnode, f.access or reg.access)
            # Theo thứ tự element của SVD, resetValue đứng trước modifiedWriteValues.
            order = {n: i for i, n in enumerate(("name", "description", "addressOffset", "size",
                     "access", "resetValue", "modifiedWriteValues", "readAction", "fields"))}
            rnode[:] = sorted(rnode, key=lambda child: order[child.tag])
    ET.indent(root, space="  ")
    return ET.tostring(root, encoding="unicode", xml_declaration=True) + "\n"


def generate(device, repo_root):
    directory = Path(repo_root) / "arch/RISCV/Device/FTEL/FX1/Include"
    generated = Path(repo_root) / "mcu-description/generated"
    generated.mkdir(parents=True, exist_ok=True)
    files = {directory / "fx1_registers.h": register_header(device),
             generated / "FX1.svd": svd(device)}
    for path, content in files.items():
        path.write_text(content, encoding="utf-8")
    return [str(path) for path in files]
