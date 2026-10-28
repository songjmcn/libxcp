#!/usr/bin/env python3
"""XCPlite V1.2 TCP/UDP 真实跨进程验收探针。

该测试只使用 Python 标准库，故意不复用主站 TUI；它直接按 XCPlite V1.2
以太网传输层的 4 字节头发送 CONNECT/GET_STATUS/GET_COMM_MODE_INFO/
DISCONNECT，验证 libxcp 的传输实现与真实 XCPlite V1.2 从站之间的最小
互操作闭环。V1.2 的 0xFC 是 SYNCH 且按规范返回错误，不把后续版本的
POLL 误用于本验收。
"""

from __future__ import annotations

import argparse
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path


_HEADER = struct.Struct("<HH")
_CONNECT = bytes((0xFF, 0x00))
_GET_STATUS = bytes((0xFD,))
# XCPlite V1.2 没有后续版本的 POLL；用 GET_COMM_MODE_INFO 验证连接后的命令通路。
_GET_COMM_MODE_INFO = bytes((0xFB,))
_DISCONNECT = bytes((0xFE,))


class ProbeFailure(RuntimeError):
    """表示 V1.2 真实验收阶段失败。"""


class SlaveOutput:
    """异步保存从站输出，避免子进程管道阻塞。"""

    def __init__(self, process: subprocess.Popen[str]) -> None:
        self._lines: list[str] = []
        self._lock = threading.Lock()
        self._threads: list[threading.Thread] = []
        for stream in (process.stdout, process.stderr):
            assert stream is not None
            thread = threading.Thread(target=self._read, args=(stream,), daemon=True)
            thread.start()
            self._threads.append(thread)

    def _read(self, stream) -> None:
        for line in stream:
            with self._lock:
                self._lines.append(line.rstrip())

    def text(self) -> str:
        with self._lock:
            return "\n".join(self._lines)


def _run_checked(command: list[str], cwd: Path) -> str:
    """运行一次短命令并在失败时保留完整输出。"""
    result = subprocess.run(
        command,
        cwd=cwd,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=30,
        check=False,
    )
    output = (result.stdout or "") + (result.stderr or "")
    if result.returncode != 0:
        raise ProbeFailure(
            f"命令失败（exit={result.returncode}）：{' '.join(command)}\n{output}"
        )
    return output


def _validate_a2l(path: Path, transport: str) -> None:
    """检查生成的 A2L 端点和标定写权限。"""
    text = path.read_text(encoding="utf-8", errors="replace")
    expected = "XCP_ON_TCP_IP" if transport == "tcp" else "XCP_ON_UDP_IP"
    if expected not in text:
        raise ProbeFailure(f"A2L 缺少 {expected}：{path}")
    if "READ_WRITE" not in text:
        raise ProbeFailure(f"A2L 缺少标定 READ_WRITE：{path}")
    if "XCP_104.aml" not in text:
        raise ProbeFailure(f"A2L 缺少 XCP_104.aml 外链：{path}")


def _frame(packet: bytes, ctr: int = 0) -> bytes:
    """生成 XCPlite V1.2 `[DLC:uint16][CTR:uint16][packet]` 帧。"""
    return _HEADER.pack(len(packet), ctr) + packet


def _recv_exact(stream: socket.socket, size: int) -> bytes:
    """从 TCP 字节流读取指定数量，处理任意 recv 分片。"""
    result = bytearray()
    while len(result) < size:
        chunk = stream.recv(size - len(result))
        if not chunk:
            raise ProbeFailure("TCP 对端提前关闭连接")
        result.extend(chunk)
    return bytes(result)


def _recv_frame(sock: socket.socket, transport: str) -> bytes:
    """接收并校验一帧 V1.2 响应。"""
    if transport == "tcp":
        header = _recv_exact(sock, _HEADER.size)
        length, _ctr = _HEADER.unpack(header)
        if length == 0 or length > 248:
            raise ProbeFailure(f"TCP 响应长度非法：{length}")
        return _recv_exact(sock, length)

    data = sock.recv(4096)
    if len(data) < _HEADER.size:
        raise ProbeFailure("UDP 响应小于 4 字节传输头")
    length, _ctr = _HEADER.unpack_from(data)
    packet = data[_HEADER.size :]
    if length != len(packet) or length == 0 or length > 248:
        raise ProbeFailure(
            f"UDP 响应 DLC 不一致：DLC={length}, 实际={len(packet)}"
        )
    return packet


def _send_and_receive(
    sock: socket.socket, transport: str, packet: bytes, ctr: int
) -> bytes:
    """发送一个 XCP 命令并读取响应，拒绝标准错误响应。"""
    sock.sendall(_frame(packet, ctr))
    response = _recv_frame(sock, transport)
    if response[0] == 0xFE:
        code = response[1] if len(response) > 1 else 0
        raise ProbeFailure(f"XCPlite 返回错误响应：0x{code:02X}")
    return response


def _probe_socket(transport: str, port: int) -> None:
    """完成 CONNECT、GET_STATUS、GET_COMM_MODE_INFO、DISCONNECT 最小闭环。"""
    sock_type = socket.SOCK_STREAM if transport == "tcp" else socket.SOCK_DGRAM
    sock = socket.socket(socket.AF_INET, sock_type)
    sock.settimeout(1.0)
    try:
        if transport == "tcp":
            sock.connect(("127.0.0.1", port))
        else:
            sock.connect(("127.0.0.1", port))

        connect_response = _send_and_receive(sock, transport, _CONNECT, 0)
        if connect_response[0] != 0xFF or len(connect_response) < 8:
            raise ProbeFailure(
                f"CONNECT 响应非法：{connect_response.hex(' ')}"
            )

        status_response = _send_and_receive(sock, transport, _GET_STATUS, 1)
        if status_response[0] != 0xFF:
            raise ProbeFailure(
                f"GET_STATUS 响应非法：{status_response.hex(' ')}"
            )

        comm_mode_response = _send_and_receive(sock, transport, _GET_COMM_MODE_INFO, 2)
        if comm_mode_response[0] != 0xFF:
            raise ProbeFailure(
                f"GET_COMM_MODE_INFO 响应非法：{comm_mode_response.hex(' ')}"
            )

        _send_and_receive(sock, transport, _DISCONNECT, 3)
    finally:
        sock.close()


def _wait_for_transport(transport: str, port: int) -> None:
    """在从站启动窗口内重试真实连接。"""
    deadline = time.monotonic() + 10.0
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            _probe_socket(transport, port)
            return
        except (OSError, ProbeFailure) as error:
            last_error = error
            time.sleep(0.1)
    raise ProbeFailure(f"{transport.upper()} 真实验收超时：{last_error}")


def run(slave: Path, transport: str, port: int, workdir: Path) -> None:
    """运行一次 A2L + XCPlite V1.2 传输验收。"""
    output_dir = Path(tempfile.mkdtemp(prefix=f"xcp-v12-{transport}-", dir=workdir))
    process: subprocess.Popen[str] | None = None
    output: SlaveOutput | None = None
    probe_error: Exception | None = None
    try:
        a2l = output_dir / "xcp_slave_demo_v12.a2l"
        generate = [
            str(slave),
            "--generate-a2l",
            "--overwrite",
            "--verify-a2l",
            "--transport",
            transport,
            "--bind",
            "127.0.0.1",
            "--port",
            str(port),
            "--output-dir",
            str(output_dir),
        ]
        _run_checked(generate, workdir)
        _run_checked(generate, workdir)
        _validate_a2l(a2l, transport)

        process = subprocess.Popen(
            [
                str(slave),
                "--transport",
                transport,
                "--bind",
                "127.0.0.1",
                "--port",
                str(port),
                "--output-dir",
                str(output_dir),
                "--ready-token",
                f"XCP_V12_{transport.upper()}_READY",
                "--max-runtime-ms",
                "30000",
            ],
            cwd=workdir,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        output = SlaveOutput(process)
        try:
            _wait_for_transport(transport, port)
        except Exception as error:
            probe_error = error
        if probe_error is None:
            print(f"PASS {transport.upper()} XCPlite V1.2 CONNECT/STATUS/COMM_MODE/DISCONNECT")
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        detail = output.text() if output is not None else ""
        shutil.rmtree(output_dir, ignore_errors=True)
        if probe_error is not None:
            raise ProbeFailure(f"{probe_error}\n从站输出：\n{detail}")
        if process is not None and process.returncode not in (0, None, 1):
            raise ProbeFailure(
                f"从站异常退出：exit={process.returncode}\n{detail}"
            )


def main() -> int:
    """解析参数并执行单一传输模式验收。"""
    parser = argparse.ArgumentParser()
    parser.add_argument("--slave", type=Path, required=True)
    parser.add_argument("--transport", choices=("tcp", "udp"), required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--workdir", type=Path, default=Path.cwd())
    args = parser.parse_args()
    try:
        run(args.slave.resolve(), args.transport, args.port, args.workdir.resolve())
    except (OSError, ProbeFailure, subprocess.SubprocessError) as error:
        print(f"FAIL {args.transport.upper()} XCPlite V1.2: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
