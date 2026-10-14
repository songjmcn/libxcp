# XCP Master TUI Tool

`xcp_master` is a standalone C++20 terminal UI tool targeting Windows, Linux, and macOS. It browses an A2L database and connects to an explicitly supplied XCP-on-UDP slave. Its display panel and command input are separated; asynchronous polling/DAQ output is routed to the UI rather than printed over active input. It does not launch or depend on XCPlite `cpp_demo` and does not reuse source code from `examples/`. Full platform builds still depend on a Boost/A2L toolchain whose C++ ABI matches the compiler.

## Build

```powershell
cmake -S . -B build -DLIBXCP_BUILD_TOOLS=ON -DLIBXCP_BUILD_TESTS=ON
cmake --build build --config Release --target xcp_master
ctest --test-dir build -C Release --output-on-failure
```

Disable the tool with `-DLIBXCP_BUILD_TOOLS=OFF` (the default). This option is independent of `LIBXCP_BUILD_EXAMPLES` and does not enable XCPlite. When tools are enabled, CMake fetches FTXUI v7.0.3 at a pinned commit using FetchContent; it is not a submodule and is not stored in `thirdparty/`. For offline builds, set CMake's standard `FETCHCONTENT_SOURCE_DIR_FTXUI` override to a matching local checkout. The A2L bridge runtime library is copied beside the executable by the tool target. The project-owned `thirdparty/a2l-sdk` integration explicitly links Boost.Locale and uses it for A2L log timestamps; provide a Boost package built for the same compiler ABI.

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
- In poll mode, `watch` reads selected symbols immediately and then polls at the requested target interval. The observation view keeps one latest timestamped value/status row per selected symbol; stopping preserves those rows and marks them stopped. Actual cycle time depends on ECU response time and number of selected symbols. This is not a hard real-time frequency guarantee.
- In DAQ mode, `watch` uses Slave event-driven DTOs; no polling interval is applied. Only scalar `MEASUREMENT` symbols with exactly one distinct A2L `FIXED_EVENT_LIST` event are eligible. Event ID 0 is valid; DEFAULT/AVAILABLE lists are never substituted. The `generic` profile supports only the validated, timestamp-free standard Absolute-PID layout, FREE address-extension mode, and one DAQ list. The explicitly selected `xcplite` profile is a separate vendor compatibility path; it validates runtime processor/resolution/event facts and does not imply generic support.
- Before each DAQ configuration, the tool performs local layout and capability checks. It then warns that `FREE_DAQ` clears the Slave's entire dynamic DAQ table and requires a fresh `y` confirmation. Refusal sends no destructive configuration command. Existing Slave DAQ configuration is not restored after Stop.
- During DAQ, `read`, `write`, `add`, and `remove` are refused; stop first. After DAQ stops, manual reads and the normal confirmed write/read-back flow are available again.
- `stop` then Enter: stop the active polling or DAQ session. Polling waits for any in-flight XCP command to return or time out.
- `write <module::name> <physical-value>`: request a scalar CHARACTERISTIC/VALUE write. The tool displays current and target values and asks for explicit confirmation; it then writes and reads back.
- `find [pattern]`: search scalar `CHARACTERISTIC/VALUE` calibration items in the separate calibration view. The view also shows read-only/unsupported candidates and their policy reason; selecting an item does not access the ECU.
- `help`, `quit`.

The upper display panel switches among observation values, calibration, command results, and logs (press F2 to cycle, or use the view menu); the command input remains fixed below. Use ↑/↓ while the command input is focused to browse up to 200 submitted commands; moving down past the newest entry restores the draft that was present before browsing. F5 requests stop; Esc returns focus to the command input. In the calibration view, use the search field, select a scalar item, enter a physical target, then explicitly request read or write. Every write follows the same conservative validation, pre-read, default-cancel confirmation, write, and read-back path as the `write` command. During confirmation, only `Y/y` confirms; `N/n` or `Esc` cancels, F5 cancels and requests stop, and other input is swallowed so it cannot reach the command field. Ordinary commands are not queued while another is running; `stop` and `quit` are prioritized as control requests. Asynchronous watch results update the display without overwriting command input. Observation, command-result, and log views retain at most the latest 512 lines each. The UI requires an interactive terminal.

## Safety and capability limits

A2L presence and the A2L `READ_WRITE` flag are metadata, not proof that a connected ECU permits access. Reads/writes are conservatively refused for non-scalar layouts, unknown widths, bit fields, addresses outside the 32-bit XCP range, incompatible address granularity, or A2L/ECU byte-order disagreement. Writes are limited to invertible scalar CHARACTERISTIC/VALUE symbols and always require confirmation. ECU-side protection can still reject writes; the tool does not invent or automatically run a Seed&Key algorithm.

DAQ is deliberately not advertised as compatible with arbitrary XCP slaves. Unknown/unverified DTO headers, timestamp modes, missing/ambiguous event declarations, insufficient resources, or mismatched runtime facts are rejected rather than decoded speculatively. The generic profile does not use XCPlite's proprietary DTO marker, first-ODT timestamp convention, or timestamp unit mapping. TCP/CAN, static PREDEFINED DAQ, page switching, arrays, curves/maps, structures, and automatic ECU management are outside this tool's supported scope.

Only connect to an ECU for which you have authorization. Use a non-critical calibration value for manual write testing and follow the ECU owner's procedure for restoring it. Automated tests do not constitute validation against every ECU; manual Slave validation is optional and was not part of the build/test suite.
