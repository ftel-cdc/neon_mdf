"""Load + sanity-check a devices/*.yaml file into the model.Device dataclass.

No jsonschema dependency on purpose (see schema/*.yaml for the documented
shape) - just enough validation here to fail loudly and clearly on the
mistakes that would otherwise silently produce a broken header/linker
script (duplicate addresses, duplicate IRQ numbers, missing fields).
"""
import re

import yaml

from . import model


class DeviceDescriptionError(ValueError):
    pass


def _require(d: dict, key: str, ctx: str):
    if key not in d:
        raise DeviceDescriptionError(f"Missing mandatory field '{key}' in... {ctx}")
    return d[key]


def load_device(path: str, profile: str = "normal") -> model.Device:
    with open(path, "r", encoding="utf-8") as f:
        raw = yaml.safe_load(f)

    device = _require(raw, "device", "root")
    vendor = _require(raw, "vendor", "root")
    family = _require(raw, "family", "root")
    arch = _require(raw, "arch", "root")
    description = raw.get("description", "")

    cpu_raw = dict(_require(raw, "cpu", "root"))
    core = _require(cpu_raw, "core", "cpu")
    revision = cpu_raw.pop("revision", "unknown")
    max_freq_hz = _require(cpu_raw, "max_freq_hz", "cpu")
    cpu_raw.pop("core", None)
    cpu_raw.pop("max_freq_hz", None)
    cpu = model.Cpu(core=core, revision=revision, max_freq_hz=max_freq_hz, extra=cpu_raw)

    memory = []
    for i, m in enumerate(raw.get("memory", [])):
        ctx = f"memory[{i}]"
        memory.append(model.MemoryRegion(
            name=_require(m, "name", ctx),
            kind=_require(m, "kind", ctx),
            address=_require(m, "address", ctx),
            size=_require(m, "size", ctx),
        ))
    _check_no_overlap(memory)

    peripherals = []
    seen_periph_base = {}
    for i, p in enumerate(raw.get("peripherals", [])):
        ctx = f"peripherals[{i}] ({p.get('name', '?')})"
        name = _require(p, "name", ctx)
        base = _require(p, "base", ctx)
        if base in seen_periph_base:
            raise DeviceDescriptionError(
                f"Peripheral '{name}duplicate base address 0x{base:08X} with '{seen_periph_base[base]}'"
            )
        seen_periph_base[base] = name

        registers = []
        seen_offsets = {}
        for j, r in enumerate(p.get("registers", [])):
            rctx = f"{ctx}.registers[{j}]"
            rname = _require(r, "name", rctx)
            offset = _require(r, "offset", rctx)
            if offset in seen_offsets:
                raise DeviceDescriptionError(
                    f"Peripheral '{name}': register '{rname}duplicate offset 0x{offset:X} with '{seen_offsets[offset]}'"
                )
            seen_offsets[offset] = rname
            def load_fields(raw_fields, fields_ctx):
                fields = [
                    model.Field(
                        name=_require(fl, "name", f"{fields_ctx}[{k}]"),
                        bit_offset=_require(fl, "bit_offset", f"{fields_ctx}[{k}]"),
                        bit_width=_require(fl, "bit_width", f"{fields_ctx}[{k}]"),
                        description=fl.get("description", ""),
                        access=fl.get("access"),
                        reset=fl.get("reset"),
                        mask=fl.get("mask"),
                    )
                    for k, fl in enumerate(raw_fields)
                ]
                field_bits = 0
                field_names = set()
                for k, fld in enumerate(fields):
                    fctx = f"{fields_ctx}[{k}] ({fld.name})"
                    if fld.name in field_names:
                        raise DeviceDescriptionError(
                            f"{fields_ctx}: field '{fld.name}' declared duplicate"
                        )
                    field_names.add(fld.name)
                    if fld.bit_offset < 0 or fld.bit_width <= 0:
                        raise DeviceDescriptionError(
                            f"{fctx}: bit_offset right >= 0 and bit_width right > 0"
                        )
                    if fld.bit_offset + fld.bit_width > r.get("size", 32):
                        raise DeviceDescriptionError(
                            f"{fctx}: bitfield exceeds register width {r.get('size', 32)} bit"
                        )
                    fld_mask = ((1 << fld.bit_width) - 1) << fld.bit_offset
                    if fld.mask is not None and (fld.mask <= 0 or fld.mask & ~fld_mask):
                        raise DeviceDescriptionError(f"{fctx}: mask exceeds field width or is empty")
                    if field_bits & fld_mask:
                        raise DeviceDescriptionError(
                            f"{fctx}: bitfield overlaps with preceding field"
                        )
                    field_bits |= fld_mask
                return fields

            fields_ = load_fields(r.get("fields", []), f"{rctx}.fields")
            aliases = []
            alias_names = set()
            for k, alias in enumerate(r.get("aliases", [])):
                actx = f"{rctx}.aliases[{k}]"
                alias_name = _require(alias, "name", actx)
                if alias_name in alias_names:
                    raise DeviceDescriptionError(
                        f"{rctx}: register alias '{alias_name}' declared duplicate"
                    )
                alias_names.add(alias_name)
                aliases.append(model.RegisterAlias(
                    name=alias_name,
                    access=alias.get("access", r.get("access", "rw")),
                    reset=alias.get("reset", r.get("reset", 0)),
                    description=alias.get("description", ""),
                    fields=load_fields(alias.get("fields", []), f"{actx}.fields"),
                ))
            registers.append(model.Register(
                name=rname,
                offset=offset,
                size=r.get("size", 32),
                access=r.get("access", "rw"),
                reset=r.get("reset", 0),
                description=r.get("description", ""),
                fields=fields_,
                aliases=aliases,
            ))
        registers.sort(key=lambda r: r.offset)
        peripherals.append(model.Peripheral(
            name=name, base=base, description=p.get("description", ""), registers=registers,
            type=p.get("type", name),
        ))

    _check_shared_type_register_layout(peripherals)

    interrupts = []
    seen_irq_numbers = {}
    for i, irq in enumerate(raw.get("interrupts", [])):
        ctx = f"interrupts[{i}]"
        number = _require(irq, "number", ctx)
        name = _require(irq, "name", ctx)
        if number in seen_irq_numbers:
            raise DeviceDescriptionError(
                f"IRQ number {number} duplicate between '{name}\" And... \" '{seen_irq_numbers[number]}'"
            )
        seen_irq_numbers[number] = name
        interrupts.append(model.Interrupt(number=number, name=name, description=irq.get("description", "")))
    interrupts.sort(key=lambda i: i.number)

    num_irq = cpu.extra.get("num_irq")
    if num_irq is not None and interrupts and interrupts[-1].number >= num_irq:
        raise DeviceDescriptionError(
            f"interrupts have number={interrupts[-1].number} >= cpu.num_irq={num_irq}"
        )

    bootloader = None
    if "bootloader" in raw:
        b_raw = raw["bootloader"]
        bl_size = _require(b_raw, "size", "bootloader")
        header_size = _require(b_raw, "image_header_size", "bootloader")
        uart_raw = _require(b_raw, "uart", "bootloader")
        bootloader = model.Bootloader(
            size=bl_size,
            image_header_size=header_size,
            uart=model.BootloaderUart(
                peripheral=_require(uart_raw, "peripheral", "bootloader.uart"),
                tx_pin=_require(uart_raw, "tx_pin", "bootloader.uart"),
                rx_pin=_require(uart_raw, "rx_pin", "bootloader.uart"),
                af=_require(uart_raw, "af", "bootloader.uart"),
                baud=_require(uart_raw, "baud", "bootloader.uart"),
            ),
        )

    erase_sectors = None
    if "erase_sectors" in raw:
        erase_sectors = []
        for i, es in enumerate(raw["erase_sectors"]):
            ctx = f"erase_sectors[{i}]"
            erase_sectors.append(model.EraseSector(
                size=_require(es, "size", ctx),
                count=_require(es, "count", ctx),
            ))
        _check_erase_sectors_cover_flash(memory, erase_sectors)

    if "privice_parram" in raw:
        raise DeviceDescriptionError("privice_parram is no longer supported; use profile_param")
    profiles = _load_profiles(raw, memory)
    if profile not in profiles:
        raise DeviceDescriptionError(f"Unknown profile '{profile}'; supported: {', '.join(profiles)}")
    result = model.Device(
        device=device, vendor=vendor, family=family, arch=arch, description=description,
        cpu=cpu, memory=memory, peripherals=peripherals, interrupts=interrupts,
        bootloader=bootloader, erase_sectors=erase_sectors, profile=profiles[profile],
    )
    from dataclasses import replace
    from .memory_layout import validate_profile, pick_flash
    for params in profiles.values():
        candidate = replace(result, profile=params)
        try:
            validate_profile(candidate)
            if bootloader and not params.flash:
                selected_memory = [pick_flash(candidate)] + candidate.memory_by_kind("ram")
                _check_bootloader_vtor_alignment(selected_memory, cpu, bootloader, arch)
        except ValueError as exc:
            raise DeviceDescriptionError(f"profile '{params.name}': {exc}") from exc
    return result


def _load_profiles(raw, memory):
    # Devices without explicit profiles retain their normal memory selection.
    if "profile_param" not in raw:
        return {"normal": model.ProfileParam()}
    entries = raw["profile_param"]
    if not isinstance(entries, list) or not entries:
        raise DeviceDescriptionError("profile_param must be a nonempty list")
    profiles = {}
    for entry in entries:
        if not isinstance(entry, dict):
            raise DeviceDescriptionError("profile_param entries must be mappings")
        allowed = {"name", "flash", "ram"}
        if entry.keys() - allowed:
            raise DeviceDescriptionError(f"profile_param: unknown fields {sorted(entry.keys() - allowed)}")
        name = _require(entry, "name", "profile_param")
        if not isinstance(name, str) or name not in ("normal", "xip"):
            raise DeviceDescriptionError(f"Unknown profile '{name}'; supported: normal, xip")
        if name in profiles:
            raise DeviceDescriptionError(f"Duplicate profile '{name}'")
        regions = {}
        for kind in ("flash", "ram"):
            block = _require(entry, kind, f"profile '{name}'")
            regions[kind] = _load_profile_memory(block, kind, memory, f"profile '{name}'.{kind}")
        profiles[name] = model.ProfileParam(name=name, **regions)
    return profiles


def _load_profile_memory(block, kind, memory, ctx):
    if not isinstance(block, dict) or block.keys() - {"region", "description", "layout"}:
        raise DeviceDescriptionError(f"{ctx}: expected region, description, layout")
    region_name = _require(block, "region", ctx)
    regions = [m for m in memory if m.name == region_name and m.kind == kind]
    if not isinstance(region_name, str) or len(regions) != 1:
        raise DeviceDescriptionError(f"{ctx}.region: must name one {kind} region")
    region = regions[0]
    description = block.get("description", "")
    if not isinstance(description, str):
        raise DeviceDescriptionError(f"{ctx}.description must be a string")
    raw_entries = _require(block, "layout", ctx)
    if not isinstance(raw_entries, list) or not raw_entries:
        raise DeviceDescriptionError(f"{ctx}.layout must be a nonempty list")
    entries, names = [], set()
    for raw_entry in raw_entries:
        if not isinstance(raw_entry, dict) or raw_entry.keys() - {"name", "offset", "size", "usage", "description"}:
            raise DeviceDescriptionError(f"{ctx}.layout: expected name, offset, size, usage, description")
        name = _require(raw_entry, "name", ctx)
        if not isinstance(name, str) or not re.fullmatch(r"[a-z][a-z0-9_]*", name):
            raise DeviceDescriptionError(f"{ctx}.layout.name must be a lowercase identifier")
        if name in names:
            raise DeviceDescriptionError(f"{ctx}.layout: duplicate name '{name}'")
        names.add(name)
        entry_ctx = f"{ctx}.layout.{name}"
        offset = _require(raw_entry, "offset", entry_ctx)
        size = _require(raw_entry, "size", entry_ctx)
        usage = _require(raw_entry, "usage", entry_ctx)
        if type(offset) is not int or offset < 0:
            raise DeviceDescriptionError(f"{entry_ctx}.offset must be a nonnegative integer")
        if size == "remaining":
            size = region.size - offset
        if type(size) is not int or size <= 0:
            raise DeviceDescriptionError(f"{entry_ctx}.size must be positive bytes or 'remaining'")
        if offset + size > region.size:
            raise DeviceDescriptionError(f"{entry_ctx}: range exceeds region {region.name}")
        if usage not in ("reserved", "allocatable"):
            raise DeviceDescriptionError(f"{entry_ctx}.usage must be reserved or allocatable")
        desc = raw_entry.get("description", "")
        if not isinstance(desc, str):
            raise DeviceDescriptionError(f"{entry_ctx}.description must be a string")
        if name == "application" and usage != "allocatable":
            raise DeviceDescriptionError(f"{entry_ctx}: application must be allocatable")
        if name in ("image_header", "irq_relay"):
            expected_kind = "flash" if name == "image_header" else "ram"
            if kind != expected_kind or usage != "reserved":
                raise DeviceDescriptionError(f"{entry_ctx}: must be reserved in {expected_kind}")
        entries.append(model.LayoutEntry(name, offset, size, usage, desc))
    if "application" not in names:
        raise DeviceDescriptionError(f"{ctx}.layout must contain an allocatable 'application' entry")
    entries.sort(key=lambda entry: entry.offset)
    for left, right in zip(entries, entries[1:]):
        if left.offset + left.size > right.offset:
            raise DeviceDescriptionError(f"{ctx}.layout: '{left.name}' overlaps '{right.name}'")
    return model.ProfileMemory(region_name, entries, description)


def _check_erase_sectors_cover_flash(memory, erase_sectors):
    """`erase_sectors` is a HAND declaration (not made of `memory:`, the uneven micromap sectors of many
    chips cannot silently omit the field. Catch missing or mistyped values early
    sectors, check that the sum of the don port sizes of all groups must match exactly with the
    maximum flash area size (the same convention as _pick_flash() in gen_linker_script.py) -
    mismatch means
    missing or extra sectors are an error."""
    flash_regions = [m for m in memory if m.kind == "flash"]
    if not flash_regions:
        raise DeviceDescriptionError("erase_sectors: device has no memory region kind=flash")
    flash = sorted(flash_regions, key=lambda m: m.address)[0]

    total = sum(es.size * es.count for es in erase_sectors)
    if total != flash.size:
        raise DeviceDescriptionError(
            f"erase_sectors: total size of groups (0x{total:X}) size mismatch "
            f"'flash area'{flash.name}' (0x{flash.size:X}) - recheck using sector, "
            f"missing or extra groups"
        )


def _check_bootloader_vtor_alignment(memory, cpu, bootloader, arch):
    """SCB->VTOR (ARMv7-M) - or equivalent vector table address of PFIC on RISC-V/WCH (mtvec[1:0]=3
    "Address" mode, see CH32V003F4P6.yaml's cpu.mtvec_mode) - requires write address must be by the
    the vector-table byte size rounded up to a power of two, at least 128 bytes. Hardware silently
    rounds misaligned VTOR values down without a fault. If image_header_size does not satisfy
    small, APP_VECTOR_BASE will not align use, vector table will point wrong value (alias vao header
    region). Any app using interrupts such as SysTick/PendSV/SVC would fetch the wrong vector -
    hardfault or undefined behavior silently, only loud when there is real interrupt
    enabled.

    the number of slot vector tables (num_vectors) calculated differently from real numbers between 2
    This fixes the earlier unconditional use of 16 + num_irq for both architectures; see the
    word-present in CH32V003F4P6.yaml assigned
    bootloader.image_header_size):
      - Cortex-M (ARMv6-M/ARMv7-M+): 16 core exception specified (same
        core vectors, from Initial SP through SysTick_Handler, followed by cpu.num_irq device vectors:
        num_vectors = 16 + num_irq.
      - RISC-V/PFIC: cpu.num_irq according to the convention of this SDK (see backends/riscv/gen_startup.py)
        is the total number of real SLOTs of the vector table, having reported GOM leveling the
        section core/reserved (PFIC only has 12 slots with headers
        table: entry 0 is "j handle_reset", 1 reserved, 2 NMI, 3 HardFault -
        not 16 as ARM) -> num_vectors = num_irq directly, not
        add 16.
    For the current CH32V003F4P6, the old false real port (16+39=55) and the used real port (39) are
    evenly spaced up to the same length (256 bytes) so they do not have a true bug - but are
    composite arithmetic, not equal to the common real port, and the real risk for the next RISC-V
    device has another num_irq.

    the number of exception/IRQ used to compute must be the real hardware power of silicon
    (cpu.extra.num_irq), not the IRQ declared in `interrupts:` (maybe empty/not enough) - the vector
    table slot must be sufficient for all possible hardware values, not just the SDK wired up here.
    """
    num_irq = cpu.extra.get("num_irq")
    if num_irq is None:
        raise DeviceDescriptionError(
            "bootloader: requires cpu.num_irq (to calculate vector table alignment "
            "real) - this device has not declared cpu.num_irq"
        )
    if arch.startswith("cortex-m"):
        num_vectors = 16 + num_irq  # 16 core exception with default + device IRQ
    elif arch.startswith("riscv"):
        num_vectors = num_irq  # cpu.num_irq is already the total number of real slots (see docstring)
    else:
        raise DeviceDescriptionError(
            f"bootloader: unknown how to calculate the number of slot vector tables for "
            f"arch '{arch}' - add fast each in _check_bootloader_vtor_alignment() "
            "(mcu_desc/loader.py) after confirming the real number of slots core/reserved specified "
            "It's your watch, no guessing."
        )
    required_align = 0x80
    while required_align < num_vectors * 4:
        required_align *= 2

    flash_regions = [m for m in memory if m.kind == "flash"]
    if not flash_regions:
        raise DeviceDescriptionError("bootloader: device without memory region kind=flash")
    flash = sorted(flash_regions, key=lambda m: m.address)[0]

    app_vector_base = flash.address + bootloader.size + bootloader.image_header_size
    if app_vector_base % required_align != 0:
        vector_formula = f"16 core + cpu.num_irq={num_irq}" if arch.startswith("cortex-m") else f"cpu.num_irq={num_irq} (was total number of slots, see docstring)"
        raise DeviceDescriptionError(
            f"bootloader.image_header_size=0x{bootloader.image_header_size:X} mismatch: "
            f"APP_VECTOR_BASE=0x{app_vector_base:08X} (flash.address+bootloader.size+image_header_size) "
            f"must be by the number of 0x{required_align:X} (requires align vector table for {num_vectors} "
            f"vector = {vector_formula}) - it is image_header_size to apply this gene"
        )


def _check_no_overlap(regions):
    sorted_regions = sorted(regions, key=lambda m: m.address)
    for a, b in zip(sorted_regions, sorted_regions[1:]):
        if a.address + a.size > b.address:
            raise DeviceDescriptionError(
                f"Memory region '{a.name}' (0x{a.address:08X}+0x{a.size:X}) overlapping. '{b.name}' (0x{b.address:08X})"
            )


def _register_signature(registers):
    """(name, offset, size, access) each register - enough to match layouts between instances of the same
    type, ignoring description/reset (no image influenced by C struct shape)."""
    return tuple((r.name, r.offset, r.size, r.access) for r in registers)


def _check_shared_type_register_layout(peripherals):
    """Peripheral with `type` (e.g. UART0..UART4 -> type UART) generates COMMON 1 C struct (see
    gen_cmsis_header.py) - if register layout deviates between instances, the generated struct will
    be wrong for at least 1 instance. Early blocking is seen here because bugs are silently revealed
    when building/running."""
    by_type = {}
    for p in peripherals:
        by_type.setdefault(p.type, []).append(p)

    for type_name, group in by_type.items():
        if len(group) < 2:
            continue
        first = group[0]
        first_sig = _register_signature(first.registers)
        for other in group[1:]:
            if _register_signature(other.registers) != first_sig:
                raise DeviceDescriptionError(
                    f"Peripheral '{other.name}\" And... \" '{first.name}' same type='{type_name}' "
                    f"but register layout is different - must be identical (jointly used YAML "
                    f"anchor, see how UART0..UART4 uses &uart_regs) because the whole group creates 1 "
                    f"struct C."
                )
