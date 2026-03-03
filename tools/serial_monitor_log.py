#!/usr/bin/env python3
"""
串口 monitor 日志采集工具。

默认启动 `idf.py -p PORT monitor`，通过伪终端采集输出并写入日志文件。
也支持用 `--command ...` 指定任意自定义命令，方便离线测试。
"""

from __future__ import annotations

import argparse
import datetime as dt
import errno
import os
import pathlib
import pty
import select
import shlex
import signal
import subprocess
import sys
from typing import Sequence


def default_log_path() -> str:
    ts = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    return str(pathlib.Path("logs") / f"monitor-{ts}.log")


def build_command(args: argparse.Namespace) -> list[str]:
    if args.command:
        return list(args.command)

    cmd = [args.idf_cmd]
    if args.port:
        cmd.extend(["-p", args.port])
    cmd.append("monitor")
    return cmd


def ensure_log_parent(path: pathlib.Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)


def write_banner(f, cmd: Sequence[str], cwd: pathlib.Path) -> None:
    ts = dt.datetime.now().isoformat(timespec="seconds")
    f.write(f"# monitor_start {ts}\n")
    f.write(f"# cwd {cwd}\n")
    f.write(f"# cmd {' '.join(shlex.quote(x) for x in cmd)}\n")
    f.flush()


def stream_process(cmd: list[str], cwd: pathlib.Path, log_path: pathlib.Path, quiet: bool) -> int:
    master_fd, slave_fd = pty.openpty()
    proc = subprocess.Popen(
        cmd,
        cwd=str(cwd),
        stdin=slave_fd,
        stdout=slave_fd,
        stderr=slave_fd,
        close_fds=True,
    )
    os.close(slave_fd)

    with log_path.open("a", encoding="utf-8", errors="replace") as log_file:
        write_banner(log_file, cmd, cwd)
        try:
            while True:
                ready, _, _ = select.select([master_fd], [], [], 0.2)
                if master_fd in ready:
                    try:
                        chunk = os.read(master_fd, 4096)
                    except OSError as exc:
                        if exc.errno == errno.EIO:
                            break
                        raise
                    if not chunk:
                        break
                    text = chunk.decode("utf-8", errors="replace")
                    log_file.write(text)
                    log_file.flush()
                    if not quiet:
                        sys.stdout.write(text)
                        sys.stdout.flush()

                if proc.poll() is not None and not ready:
                    break
        except KeyboardInterrupt:
            proc.send_signal(signal.SIGINT)
        finally:
            try:
                os.close(master_fd)
            except OSError:
                pass

        ret = proc.wait()
        log_file.write(f"\n# monitor_exit {ret}\n")
        log_file.flush()
        return ret


def parse_args() -> argparse.Namespace:
    root = pathlib.Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description="采集 idf.py monitor 输出到日志文件")
    parser.add_argument("--port", default=os.environ.get("ESPPORT", ""), help="串口号，如 /dev/ttyACM0")
    parser.add_argument("--idf-cmd", default="idf.py", help="idf.py 命令名")
    parser.add_argument("--log-file", default=default_log_path(), help="日志输出路径")
    parser.add_argument("--cwd", default=str(root), help="执行命令的工作目录")
    parser.add_argument("--quiet", action="store_true", help="只写日志，不回显到终端")
    parser.add_argument(
        "--command",
        nargs=argparse.REMAINDER,
        help="自定义命令。提供后不再启动 idf.py monitor；例如 --command python3 -c 'print(123)'",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    cwd = pathlib.Path(args.cwd).resolve()
    log_path = pathlib.Path(args.log_file).resolve()
    ensure_log_parent(log_path)

    cmd = build_command(args)
    if not cmd:
        print("缺少可执行命令。", file=sys.stderr)
        return 2

    print(f"启动 monitor 采集: log={log_path}")
    print(f"执行命令: {' '.join(shlex.quote(x) for x in cmd)}")
    return stream_process(cmd, cwd, log_path, args.quiet)


if __name__ == "__main__":
    raise SystemExit(main())
