#!/usr/bin/env python3
"""在 Release/CI 中验证 TCP/UDP 公共层不含原生 Socket API。"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

# 对 src/ 只豁免四个明确列出的原生实现，防止日后新增文件绕过门禁。
PLATFORM_FILES = frozenset(
    {
        "src/detail/socket/socket_ops_win.cpp",
        "src/detail/socket/socket_ops_posix.cpp",
        "src/detail/socket/event_poller_win.cpp",
        "src/detail/socket/event_poller_posix.cpp",
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
    if violations:
        print("平台边界违例：")
        print("\n".join(violations))
        return 1
    print(f"平台边界通过：已扫描 {len(files)} 个 src/ 非平台文件")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
