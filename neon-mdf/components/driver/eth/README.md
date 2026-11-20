# Neon Ethernet driver

Include `eth/neon_eth.h`. The public API has no RTOS, VP or reference-driver
headers. The FX1 backend is selected by family through `CMakeLists.txt`, with
`CONFIG_NEON_MDF_DRIVER_ETH` (default enabled). Other families have no backend.

```text
eth/
  neon_eth.h                         public API, opaque driver-owned handle
  CMakeLists.txt                     optional family dispatch
  platform/fx1/neon_eth.c             Neon API and single-instance ownership
  platform/fx1/reference/             imported MAC/DMA implementation, private
```

The reference files are copied from `duyphan95z/fx1`, dev commit
`3b368050398947c420f6d5f4504700e92fa0fa9c`, directory
`sw/bootloader/sources/VP_FX1_V3.0/drivers/eth/`:
`eth_drv.[ch]` and `fx1_eth.[ch]` are unchanged. Their names, comments and
format are retained under the convention's third-party-wrapper exception.
`reference/eth_regs.h` is a compatibility layer: it maps the original names
to the header generated from `mcu-description/devices/FX1.yaml`. None of these
sources is compiled from `VP/`; a source-only Neon checkout can build ELF.

The 91 register offsets, reset values and field masks were imported from that
commit's CSR workbook header. It does not specify register access semantics,
so YAML marks them `unknown`, rather than inventing RW/W1C rules. The generated
C header includes them; SVD deliberately omits unknown-access registers.
The reference driver implements the side effects required by MAS v1.0.7.

## Lifecycle

`init` acquires the sole ETH0 handle on hart 0 with defaults (locally
administered MAC `02:00:46:58:31:01`, 100 Mbps, PHY address 1). `set_config`
optionally replaces MAC/speed/PHY-loopback before the first `start`.
Register an RX callback, optionally `probe`, then `start` and `transmit`.
`process` delivers RX and reclaims TX; call it within its returned polling
interval even when no IRQ arrives. Stop before deinit; after successful deinit
all saved pointers to that handle are invalid. A later init acquires the same
static storage. Reconfigure by stop/deinit/init, not by changing live rings.

TX copies 60..1514 bytes (DA through payload, no FCS). RX callback data is
borrowed until the callback returns; copy it to retain it. DES0 is supplied
for error/checksum inspection; error/truncated frames are not guaranteed valid
network packets. Callbacks must not reenter the driver.

One main loop or externally serialized task context owns every API except
`irq_handler`. The driver is not a multicore lock. For interrupts, the app owns
the trap/PLIC dispatch: invoke `neon_eth_irq_handler` for source 6 before PLIC
complete, then wake the owner to call `process`. Lifecycle calls preserve and
mask source 6 while they rewrite interrupt enables; they do not disable all
CPU interrupts during hardware waits. The API installs no RTOS task.

DMA uses its own MAC engine, not SYS_DMA. Rings and buffers reserve 68 KiB in
`.eth_dma` (NOLOAD, 4 KiB aligned) and use offsets from DDR base. DDR must be
initialized before start. The reference HAL retains FX1 DMA ownership/barrier
hooks; these do not establish cache coherency on real silicon.

A DONE interrupt can precede the descriptor's OWN write-back. The service
keeps polling and never reuses hardware-owned memory. Stop/recover have bounded
waits; a timeout retains ownership in FAULTED. Do not free/reset/reinitialize
that memory. `recover` retries quiescence and may restart the MAC.

See [ETH_Loopback](../../../../examples/ETH_Loopback/README.md) for validation.
There is no TCP/IP stack, TAP/host networking or claim of silicon/throughput
validation. Fault recovery is retained from the reference core; the Neon
example currently validates normal lifecycle and delayed write-backs, not the
full reference fault-injection suite.
