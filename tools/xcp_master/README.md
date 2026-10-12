# XCP Master CLI Tool

`xcp_master` is a standalone C++20 command-line tool for browsing an A2L database and connecting to an explicitly supplied XCP-on-UDP slave. It does not launch or depend on XCPlite `cpp_demo` and does not reuse source code from `examples/`.

## Build

```powershell
cmake -S . -B build -DLIBXCP_BUILD_TOOLS=ON -DLIBXCP_BUILD_TESTS=ON
cmake --build build --config Release --target xcp_master
ctest --test-dir build -C Release --output-on-failure
```

Disable the tool with `-DLIBXCP_BUILD_TOOLS=OFF` (the default). This option is independent of `LIBXCP_BUILD_EXAMPLES` and does not enable XCPlite. The A2L bridge runtime library is copied beside the executable by the tool target.

## Run

```text
xcp_master --a2l <path> --host <IPv4> --port <1..65535> [--module <name>] [--mode poll|daq] [--interval-ms <ms>] [--daq-profile generic|xcplite]
```

Required arguments are the local A2L file, slave IPv4 address and UDP port. `--module` filters the symbol listing. The default is `--mode poll`; the poll target interval is 500 ms. `--interval-ms` is rejected in DAQ mode; DAQ timing is controlled by ECU events, not by this option. `--daq-profile` is valid only with DAQ mode and defaults to `generic`. Use `--help` for usage.

## Interactive commands

- `list [pattern]`: browse matching A2L names, 40 entries per page. This only queries the A2L database; it does not read live ECU values. The output includes fixed-event declarations.
- `list all`: print all symbols matching the optional module filter.
- `more`: show the next page from the last listing.
- `info <module::name>`: show symbol metadata, fixed/default/available event lists, and conservative read/write support status.
- `add <module::name>` / `remove <module::name>`: manage the selected observation set.
- `read <module::name>`: read once and convert to a physical value.
- In poll mode, `watch` reads selected symbols immediately and then polls at the requested target interval. Results append as timestamped lines; actual cycle time depends on ECU response time and number of selected symbols. This is not a hard real-time frequency guarantee.
- In DAQ mode, `watch` uses Slave event-driven DTOs; no polling interval is applied. Only scalar `MEASUREMENT` symbols with exactly one distinct A2L `FIXED_EVENT_LIST` event are eligible. Event ID 0 is valid; DEFAULT/AVAILABLE lists are never substituted. The `generic` profile supports only the validated, timestamp-free standard Absolute-PID layout, FREE address-extension mode, and one DAQ list. The explicitly selected `xcplite` profile is a separate vendor compatibility path; it validates runtime processor/resolution/event facts and does not imply generic support.
- Before each DAQ configuration, the tool performs local layout and capability checks. It then warns that `FREE_DAQ` clears the Slave's entire dynamic DAQ table and requires a fresh `y` confirmation. Refusal sends no destructive configuration command. Existing Slave DAQ configuration is not restored after Stop.
- During DAQ, `read`, `write`, `add`, and `remove` are refused; stop first. After DAQ stops, manual reads and the normal confirmed write/read-back flow are available again.
- `stop` then Enter: stop the active polling or DAQ session. Polling waits for any in-flight XCP command to return or time out.
- `write <module::name> <physical-value>`: request a scalar CHARACTERISTIC/VALUE write. The tool displays current and target values and asks for explicit confirmation; it then writes and reads back.
- `help`, `quit`.

The interface is a normal text command loop, not a split-pane TUI. Asynchronous watch results may appear among terminal input; Enter submits commands. Master commands are serialized on the single XCP connection, and DAQ callbacks enqueue output without printing on the DTO worker.

## Safety and capability limits

A2L presence and the A2L `READ_WRITE` flag are metadata, not proof that a connected ECU permits access. Reads/writes are conservatively refused for non-scalar layouts, unknown widths, bit fields, addresses outside the 32-bit XCP range, incompatible address granularity, or A2L/ECU byte-order disagreement. Writes are limited to invertible scalar CHARACTERISTIC/VALUE symbols and always require confirmation. ECU-side protection can still reject writes; the tool does not invent or automatically run a Seed&Key algorithm.

DAQ is deliberately not advertised as compatible with arbitrary XCP slaves. Unknown/unverified DTO headers, timestamp modes, missing/ambiguous event declarations, insufficient resources, or mismatched runtime facts are rejected rather than decoded speculatively. The generic profile does not use XCPlite's proprietary DTO marker, first-ODT timestamp convention, or timestamp unit mapping. TCP/CAN, static PREDEFINED DAQ, page switching, arrays, curves/maps, structures, and automatic ECU management are outside this tool's supported scope.

Only connect to an ECU for which you have authorization. Use a non-critical calibration value for manual write testing and follow the ECU owner's procedure for restoring it. Automated tests do not constitute validation against every ECU; manual Slave validation is optional and was not part of the build/test suite.
