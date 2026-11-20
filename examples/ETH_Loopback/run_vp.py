#!/usr/bin/env python3
"""Run the Neon ETH example; require firmware checks and the simulator verdict."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--vp', type=Path, required=True)
parser.add_argument('--elf', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--writeback-delay-us', type=int, default=0)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
result = {'status': 'FAIL'}
try:
    if not args.vp.is_file():
        raise ValueError('Select -DNEON_FX1_VP_EXECUTABLE=/path/to/fx1_soc (VP dev with ETH)')
    command = [str(args.vp.resolve()), '--fw', str(args.elf.resolve()),
               '--eth-peer', 'none', '--timeout-ms', '1000',
               '--eth-wb-delay-us', str(args.writeback_delay_us)]
    result['command'] = command
    proc = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, timeout=60, check=False)
    (args.output / 'platform.log').write_text(proc.stdout)
    result['exit_code'] = proc.returncode
    summary = re.search(r'\[NEON_ETH\] SUMMARY checks=(\d+) failed=0', proc.stdout)
    checks = re.findall(r'\[NEON_ETH\] PASS (\d+) ', proc.stdout)
    if (proc.returncode != 0 or '[fx1_soc] RESULT PASS' not in proc.stdout
            or summary is None or '[NEON_ETH] FAIL' in proc.stdout):
        raise ValueError('Firmware or VP failed; see platform.log')
    count = int(summary[1])
    if count < 100 or list(map(int, checks)) != list(range(1, count + 1)):
        raise ValueError('Incomplete firmware checks')
    result.update(status='PASS', checks=count)
except subprocess.TimeoutExpired as exc:
    output = exc.stdout or b''
    (args.output / 'platform.log').write_text(output.decode(errors='replace') if isinstance(output, bytes) else output)
    result['error'] = 'VP exceeded 60 seconds of host time'
except (OSError, ValueError) as exc:
    result['error'] = str(exc)
(args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
sys.exit(0 if result['status'] == 'PASS' else 1)
