"""Intermediate data model shared by every arch backend.

Kept arch-agnostic on purpose: peripheral/register/memory shape doesn't
depend on the CPU core. Only cpu.* fields vary per architecture, and each
backend (backends/cortex_m/, backends/riscv/) is responsible for knowing
which cpu.* fields it needs.
"""
from dataclasses import dataclass, field
from typing import Optional


@dataclass
class Field:
    name: str
    bit_offset: int
    bit_width: int
    description: str = ""
    access: Optional[str] = None
    reset: Optional[int] = None
    # Implemented bits can be a subset of the documented field width (FX1 DMA).
    mask: Optional[int] = None


@dataclass
class RegisterAlias:
    """A logical view of the register shares the same physical address.

    For example, UART 16550 uses the 0x00 offset for RBR when read, THR when write and DLL when DLAB=1.
    Alias keeps the name/access/bitfield correct without misusing struct C.
    """
    name: str
    access: str
    reset: int
    description: str = ""
    fields: list = field(default_factory=list)  # list[Field]


@dataclass
class Register:
    name: str
    offset: int
    size: int = 32
    access: str = "rw"
    reset: int = 0
    description: str = ""
    fields: list = field(default_factory=list)  # list[Field]
    aliases: list = field(default_factory=list)  # list[RegisterAlias]


@dataclass
class Peripheral:
    name: str
    base: int
    description: str = ""
    registers: list = field(default_factory=list)  # list[Register]
    type: str = ""  # struct name shared between multiple instances (e.g. UART0..UART4 -> "UART"). Default = name
    # (peripheral alone, same instance struct name). loader.py fill this default when field `type`
    # is not in YAML.


@dataclass
class MemoryRegion:
    name: str
    kind: str  # flash | rom | ram
    address: int
    size: int


@dataclass
class Interrupt:
    number: int
    name: str
    description: str = ""


@dataclass
class Cpu:
    core: str
    revision: str
    max_freq_hz: int
    extra: dict  # arch-specific leftover fields (nvic_priority_bits, num_irq, ...)


@dataclass
class BootloaderUart:
    peripheral: str  # peripherals[].name use UART for bootloader (e.g. "USART1")
    tx_pin: str
    rx_pin: str
    af: int
    baud: int


@dataclass
class Bootloader:
    """optional declaration: device has bootloader please not (see schema/common.schema.yaml). when it
    does, generator generates more
    <DEVICE>_BOOTLOADER.ld + <DEVICE>_APP.ld (other than <DEVICE>_FLASH.ld still generate normally) -
    see gen_linker_script.py."""
    size: int               # size partition bootloader (bytes), from the beginning of the flash
    image_header_size: int  # size "slot" header before app vector table (byte) - must be large enough for
    # flash.address+size+image_header_size to align SCB->VTOR (see loader.py
    # _check_bootloader_vtor_alignment) - not just >= actual sizeof(image_header_t).
    uart: BootloaderUart


@dataclass
class EraseSector:
    """1 sector erase-joint group, running-length (see
    schema/common.schema.yaml erase_sectors); STM32F412 has three groups:
    four 16KB sectors, one 64KB sector and three 128KB sectors, listed in increasing address order (not a
    large knife, tools/gen_flashmap.py read sequentially from the beginning of the flash to calculate the
    gioi)."""
    size: int
    count: int


@dataclass
class LayoutEntry:
    name: str
    offset: int
    size: int  # Resolved byte count; "remaining" is expanded by the loader.
    usage: str  # reserved | allocatable
    description: str = ""


@dataclass
class ProfileMemory:
    region: str
    layout: list  # list[LayoutEntry]
    description: str = ""

    def entry(self, name):
        return next((entry for entry in self.layout if entry.name == name), None)


@dataclass
class ProfileParam:
    name: str = "normal"
    flash: object = None  # Optional[ProfileMemory]; None retains legacy selection.
    ram: object = None  # Optional[ProfileMemory]


@dataclass
class Device:
    device: str
    vendor: str
    family: str
    arch: str
    description: str
    cpu: Cpu
    memory: list  # list[MemoryRegion]
    peripherals: list  # list[Peripheral]
    interrupts: list  # list[Interrupt]
    bootloader: object = None  # Optional[Bootloader] - None = device without bootloader partition
    erase_sectors: object = None  # Optional[list[EraseSector]] - None = no declarations, tools/gen_flashmap.py ignores validation of
    # sectors when devices are not at the same level (optional, only for additional validation of
    # devices with uneven sectors - see WEACT_F412.yaml).
    profile: ProfileParam = field(default_factory=ProfileParam)

    def memory_by_kind(self, kind: str):
        return [m for m in self.memory if m.kind == kind]

    def linker_filename(self, image: str):
        # Lowercase device IDs use fully lowercase filenames; legacy IDs keep
        # their existing names so older targets do not change paths.
        suffix = image.lower() if self.device.islower() else image.upper()
        return f"{self.device}_{suffix}.ld"
