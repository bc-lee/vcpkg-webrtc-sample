#!/usr/bin/env python3

import argparse
import os
import secrets
import subprocess
import sys
import threading
import time


def _drain_output(proc: subprocess.Popen[str], lines: list[str],
                  done: threading.Event) -> None:
  assert proc.stdout is not None
  try:
    for line in proc.stdout:
      lines.append(line)
  finally:
    done.set()


def collect_process(
    proc: subprocess.Popen[str],
    name: str,
    timeout: int,
    lines: list[str],
    done: threading.Event,
) -> tuple[int, str]:
  try:
    proc.wait(timeout=timeout)
  except subprocess.TimeoutExpired:
    proc.kill()
    proc.wait()
    done.wait(timeout=5)
    return 124, f"===== {name} =====\nTIMEOUT after {timeout}s\n{''.join(lines)}"

  done.wait(timeout=5)
  return proc.returncode or 0, f"===== {name} =====\n{''.join(lines)}"


def main() -> int:
  parser = argparse.ArgumentParser()
  parser.add_argument("--binary", required=True)
  parser.add_argument("--video", required=True)
  args = parser.parse_args()

  server_url = os.environ.get("WEBRTC_APPRTC_TEST_SERVER_URL")
  if not server_url:
    print("skipping e2e: WEBRTC_APPRTC_TEST_SERVER_URL is not set")
    return 0

  room_id = f"room-{secrets.token_hex(4)}"
  common = [
      args.binary,
      f"--room_server_url={server_url}",
      f"--room_id={room_id}",
      f"--video_path={args.video}",
      "--timeout_seconds=20",
      "--call_duration_seconds=4",
      "--stats_interval_seconds=1",
      "--trace_signaling",
  ]

  callee = subprocess.Popen(
      common, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
  callee_lines: list[str] = []
  callee_done = threading.Event()
  callee_thread = threading.Thread(
      target=_drain_output,
      args=(callee, callee_lines, callee_done),
      daemon=True)
  callee_thread.start()
  time.sleep(1)
  caller = subprocess.Popen(
      common, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
  caller_lines: list[str] = []
  caller_done = threading.Event()
  caller_thread = threading.Thread(
      target=_drain_output,
      args=(caller, caller_lines, caller_done),
      daemon=True)
  caller_thread.start()

  caller_code, caller_output = collect_process(caller, "caller", 40,
                                               caller_lines, caller_done)
  callee_code, callee_output = collect_process(callee, "callee", 40,
                                               callee_lines, callee_done)

  caller_thread.join(timeout=1)
  callee_thread.join(timeout=1)

  print("\n".join([caller_output, callee_output]))
  if caller_code != 0:
    return caller_code
  if callee_code != 0:
    return callee_code
  return 0


if __name__ == "__main__":
  sys.exit(main())
