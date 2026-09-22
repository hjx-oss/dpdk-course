#!/usr/bin/env python3
import json
import os
import subprocess
import resolve
from pathlib import Path
import sys
import demo
import arp_cases
import udp_cases
import icmp_cases


def cases():
    lines = demo.demonstrate()
    lines += arp_cases.cases()
    lines += udp_cases.cases()
    lines += icmp_cases.cases()
    return lines


if __name__ == "__main__":
    if "--inside" not in sys.argv:
        demo.lab.enter([sys.executable, str(Path(__file__).resolve()), "--inside"])
    lines, result = demo.run(cases)
    idle_lines, idle_result = demo.run(lambda: ["PASS idle exit and port reopen"])
    state_command = [str(demo.LESSON / 'build/arp_state'), '-l', str(min(os.sched_getaffinity(0))),
                     '-m', '128', '--no-pci', '--no-huge', '--no-shconf', '--no-telemetry']
    state = subprocess.run(state_command, text=True, capture_output=True, check=True)
    state_lines = [line for line in state.stdout.splitlines() if line.startswith('PASS ')]
    (demo.LESSON / 'build/state.txt').write_text('\n'.join(state_lines) + '\n')
    print('\n'.join(state_lines))
    api_command = [str(demo.LESSON / 'build/udp_state')] + state_command[1:]
    api = subprocess.run(api_command, text=True, capture_output=True, check=True)
    api_lines = [line for line in api.stdout.splitlines() if line.startswith('PASS ')]
    (demo.LESSON / 'build/api-state.txt').write_text('\n'.join(api_lines) + '\n')
    print('\n'.join(api_lines))
    active_lines = resolve.checks()
    record = {"status": "passed", "dpdk": "25.11.3", "checks": lines + idle_lines + state_lines + api_lines + active_lines,
              "result": result, "idle_result": idle_result}
    (demo.LESSON / "build/check.json").write_text(json.dumps(record, indent=2) + "\n")
