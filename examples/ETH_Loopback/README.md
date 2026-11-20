# FX1 Neon Ethernet loopback

This example uses only `eth/neon_eth.h` for the Ethernet driver. It checks
MAC/PHY identity, bad arguments, exclusive handle ownership, polling, PLIC
source 6, 40 frames across 16-entry ring wraps, byte-exact payloads at lengths
60/61/512/1514, stop/restart, deinit/reacquire and 10/100 Mbps PHY loopback.
It requires the FX1 VP **dev version with Ethernet**, not the older V3 binary
where ETH was reserved. PHY loopback works even with `--eth-peer none`.

From this directory, with CMake, Ninja, Python (PyYAML/kconfiglib), and
`riscv-none-elf-*` on PATH:

```bash
cmake --preset FX1
cmake --build --preset FX1
cmake --build --preset FX1 --target run-vp
```

The runner defaults to the root submodule's `VP/vp/bin/VP_FX1_V3.0/fx1_soc`.
Initialize it from the repository root with `git submodule update --init VP`.
An explicit CMake `-DNEON_FX1_VP_EXECUTABLE=/path/to/fx1_soc` or the same-named
environment variable overrides discovery. Building the ELF does not require VP.

Expected: `[NEON_ETH] SUMMARY checks=110 failed=0`, simulator
`[fx1_soc] RESULT PASS`, and exit 0. Logs and verdict are written to
`build/FX1/vp/platform.log` and `result.json`.

For a delayed descriptor write-back (DONE before OWN returns):

```bash
python3 run_vp.py --vp ../../VP/vp/bin/VP_FX1_V3.0/fx1_soc \
  --elf build/FX1/eth_loopback.elf --output build/FX1/vp-delayed \
  --writeback-delay-us 5000
```

`CONFIG_NEON_MDF_DRIVER_ETH=y` is required by this example. Disable the option
in an app that does not use ETH to remove its sources from the build.

This is functional VP validation of the Neon driver. It does not validate a
physical PHY/RMII interface, TCP/IP, host networking or line throughput.
