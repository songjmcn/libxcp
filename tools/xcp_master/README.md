# XCP Master CLI Tool

`xcp_master` is a standalone C++20 command-line tool for browsing an A2L database and connecting to a user-specified XCP-on-UDP slave. It does not launch or depend on XCPlite `cpp_demo` and does not reuse source code from `examples/`.

## Build

```powershell
cmake -S . -B build -DLIBXCP_BUILD_TOOLS=ON -DLIBXCP_BUILD_TESTS=ON
cmake --build build --config Release --target xcp_master
ctest --test-dir build -C Release -R XcpMasterTool --output-on-failure
```

Disable the tool with `-DLIBXCP_BUILD_TOOLS=OFF` (the default). This option is independent of `LIBXCP_BUILD_EXAMPLES` and does not enable XCPlite. The A2L bridge runtime library is copied beside the executable by the tool target.

## Run

```text
xcp_master --a2l <path> --host <IPv4> --port <1..65535> [--module <name>] [--interval-ms <ms>]
```

Required arguments are the local A2L file, slave IPv4 address and UDP port. `--module` filters the symbol listing. The default poll interval is 500 ms. The tool loads the A2L file, prints its symbol count and load warnings, then connects to the explicitly supplied slave. Use `--help` for usage.

## Interactive commands

- `list [pattern]`: browse matching A2L names, 40 entries per page. This only queries the A2L database; it does not read live ECU values.
- `list all`: print all symbols matching the optional module filter.
- `more`: show the next page from the last listing.
- `info <module::name>`: show symbol metadata and conservative read/write support status.
- `add <module::name>` / `remove <module::name>`: manage the polling set.
- `read <module::name>`: read once and convert to a physical value.
- `watch`: read selected symbols immediately and then poll at the requested interval. Results append as timestamped lines; actual cycle time depends on ECU response time and number of selected symbols.
- `stop` then Enter: stop the polling worker after any in-flight XCP command returns or times out.
- `write <module::name> <physical-value>`: request a scalar CHARACTERISTIC/VALUE write. The tool displays the current and target values and asks for explicit confirmation; it then writes and reads back.
- `help`, `quit`.

The interface is a normal text command loop, not a split-pane TUI. While `watch` runs, asynchronous result lines can appear among terminal input; Enter submits commands. Commands issued through the tool are serialized on the single XCP master connection.

## Safety and capability limits

A2L presence and the A2L `READ_WRITE` flag are metadata, not proof that a connected ECU permits access. Reads/writes are conservatively refused for non-scalar layouts, unknown widths, bit fields, addresses outside the 32-bit XCP range, incompatible address granularity, or A2L/ECU byte-order disagreement. Writes are limited to invertible scalar CHARACTERISTIC/VALUE symbols and always require confirmation. ECU-side access protection can still reject a write; the tool does not invent or automatically run a Seed&Key algorithm. TCP/CAN, DAQ, page switching, arrays, curves/maps, structures and automatic ECU management are not supported.

Only connect to an ECU for which you have authorization. Use a non-critical calibration value for manual write testing and follow the ECU owner's procedure for restoring it.
