#!/usr/bin/env python3
"""在 Release/CI 中验证 TCP/UDP 公共层不含原生 Socket API。"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


def force_utf8_console() -> None:
    """把标准输出/错误强制切到 UTF-8。

    Windows 上 Python 默认按 ANSI 代码页（CI 的 GitHub Actions 是 cp1252）
    编码 `print()`，本脚本的输出含中文，会抛 `UnicodeEncodeError` 让门禁
    在“全部检查通过”的情况下依然返回非零——曾真实发生过一次。
    """
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is None:
            continue
        try:
            reconfigure(encoding="utf-8", errors="replace")
        except (ValueError, OSError):
            # 流已被重定向到不支持重配置的对象时保持原样，不能因此失败。
            continue

# 对 src/ 只豁免明确列出的原生实现，防止日后新增文件绕过门禁。
# Socket/轮询原生实现四个；受控 A2L 快照的 ACL/权限原生实现两个（第二阶段提取
# 时从 tools/xcp_master 迁入，按平台二选一编译）。
PLATFORM_FILES = frozenset(
    {
        "src/detail/socket/socket_ops_win.cpp",
        "src/detail/socket/socket_ops_posix.cpp",
        "src/detail/socket/event_poller_win.cpp",
        "src/detail/socket/event_poller_posix.cpp",
        "src/detail/fs/trusted_snapshot_win.cpp",
        "src/detail/fs/trusted_snapshot_posix.cpp",
    }
)

# 命中原生头、类型、常量或调用时使 CTest 失败；忽略注释中的历史说明。
NATIVE_TOKEN = re.compile(
    r"(?:\b(?:_WIN32|sockaddr\w*|socklen_t|in_addr|addrinfo|pollfd|SOCKET|INVALID_SOCKET|"
    r"WSA\w+|FIONBIO|SHUT_RDWR|SD_BOTH|AF_INET|SOCK_STREAM|SOCK_DGRAM|"
    r"IPPROTO_TCP|IPPROTO_UDP|SO_REUSEADDR|SO_RCVTIMEO|TCP_NODELAY|"
    r"MSG_NOSIGNAL|SO_NOSIGPIPE)\b|"
    r"::(?:socket|bind|getsockname|connect|accept|shutdown|setsockopt|"
    r"getsockopt|send|recv|sendto|recvfrom|poll|select|inet_pton|"
    r"inet_ntop|getaddrinfo|freeaddrinfo|closesocket|ioctlsocket|fcntl)\s*\()"
)

# 未加 `::` 限定的原生调用同样违规（`sendto(...)` 与 `::sendto(...)` 等价）。
# 只收录公共层**没有**声明同名方法的名字，避免把 `socket_ops.hpp` 里的
# `virtual int bind/SOCKET` 之类的声明误判成调用；`ops.send_to(` 这类语义调用
# 由前置字符 `.`/`->`/`::` 排除。
UNQUALIFIED_NATIVE_CALL = re.compile(
    r"(?<![\w:.>])(?:socket|sendto|recvfrom|inet_pton|inet_ntop|getaddrinfo|"
    r"freeaddrinfo|closesocket|ioctlsocket|WSAPoll|WSASocketW|WSASocketA)\s*\("
)

STRING_LITERAL = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
COMMENT_OR_LITERAL = re.compile(
    r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/',
    re.DOTALL,
)
NATIVE_INCLUDE = re.compile(
    r"#\s*include\s*[<\"](?:windows|winsock2|ws2tcpip|mstcpip|"
    r"arpa/inet|netinet/in|netinet/tcp|sys/socket|sys/types|"
    r"sys/poll|sys/select|poll|fcntl)\.h[>\"]"
)


def main() -> int:
    """逐个扫描公共文件，打印带行号的违例并返回非零。"""
    force_utf8_console()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_root", type=Path)
    args = parser.parse_args()
    root = args.source_root.resolve()
    violations: list[str] = []
    for relative in sorted(PLATFORM_FILES):
        if not (root / relative).is_file():
            violations.append(f"{relative}: 平台实现文件缺失")
    files = sorted(
        path for path in (root / "src").rglob("*")
        if path.is_file() and path.suffix in {".hpp", ".cpp"}
        and path.relative_to(root).as_posix() not in PLATFORM_FILES
    )
    for path in files:
        relative = path.relative_to(root).as_posix()
        original = path.read_text(encoding="utf-8")
        # 保留换行数量用于行号，并避免把字符串中的 // 当成注释。
        stripped = COMMENT_OR_LITERAL.sub(
            lambda match: match.group() if match.group().startswith(("\"", "'"))
            else "\n" * match.group().count("\n"),
            original,
        )
        for match in NATIVE_INCLUDE.finditer(stripped):
            line = stripped.count("\n", 0, match.start()) + 1
            violations.append(f"{relative}:{line}: {match.group()}")
        without_literals = STRING_LITERAL.sub(
            lambda match: "\n" * match.group().count("\n"), stripped
        )
        for match in NATIVE_TOKEN.finditer(without_literals):
            line = without_literals.count("\n", 0, match.start()) + 1
            violations.append(f"{relative}:{line}: {match.group()}")
        for match in UNQUALIFIED_NATIVE_CALL.finditer(without_literals):
            line = without_literals.count("\n", 0, match.start()) + 1
            violations.append(f"{relative}:{line}: {match.group()}")
    if violations:
        print("平台边界违例：")
        print("\n".join(violations))
        return 1
    print(f"平台边界通过：已扫描 {len(files)} 个 src/ 非平台文件")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
