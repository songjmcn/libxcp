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
- `list all`: retain a complete, separately browsable snapshot of all symbols matching the optional module filter in the command-result view (not truncated to the latest 512 diagnostic lines). The next command returns that view to normal results.
- `more`: show the next page from the last listing.
- `info <module::name>`: show symbol metadata, fixed/default/available event lists, and conservative read/write support status.
- `add <module::name>` / `remove <module::name>`: manage the selected observation set.
- `read <module::name>`: read a scalar once and convert to a physical value. For a one-dimensional MEASUREMENT array, use the variable-details modal to read a 0-based element or a page of up to eight contiguous elements.
- In poll mode, `watch` reads selected symbols immediately and then polls at the requested target interval. The variable preview keeps one latest timestamped value/status row per selected symbol; stopping preserves those rows and marks them stopped. Actual cycle time depends on ECU response time and number of selected symbols. This is not a hard real-time frequency guarantee.
- In DAQ mode, `watch` uses Slave event-driven DTOs; no polling interval is applied. Only scalar `MEASUREMENT` symbols with exactly one distinct A2L `FIXED_EVENT_LIST` event are eligible. Event ID 0 is valid; DEFAULT/AVAILABLE lists are never substituted. The `generic` profile supports only the validated, timestamp-free standard Absolute-PID layout, FREE address-extension mode, and one DAQ list. The explicitly selected `xcplite` profile is a separate vendor compatibility path; it validates runtime processor/resolution/event facts and does not imply generic support.
- Before each DAQ configuration, the tool performs local layout and capability checks. It then warns that `FREE_DAQ` clears the Slave's entire dynamic DAQ table and requires a fresh `y` confirmation. Refusal sends no destructive configuration command. Existing Slave DAQ configuration is not restored after Stop.
- During DAQ, `read`, `write`, `add`, and `remove` are refused; stop first. After DAQ stops, manual reads and the normal confirmed write/read-back flow are available again.
- `stop` then Enter: stop the active polling or DAQ session. Polling waits for any in-flight XCP command to return or time out.
- `write <module::name> <physical-value>`: request a scalar CHARACTERISTIC/VALUE write. The tool displays current and target values and asks for explicit confirmation; it then writes and reads back.
- `find [pattern]`: search A2L measurements and calibration characteristics; results appear in the unified variable-preview view. Scalar `CHARACTERISTIC/VALUE` and verified fixed-axis `CHARACTERISTIC/CURVE` points may expose write actions; other candidates show their policy reason. The command and preview search box share the active filter when the result is applied. Searching only queries A2L metadata and does not access the ECU.
- `help`, `quit`.

## UI layout and keyboard

The screen has four fixed layers: a two-line status area, the main panel, a persistent command input, and a one-line context hint. The main panel has three horizontal views: **Variables**, **Command results**, and **Logs**. There are no global Watch/Stop/Quit buttons; commands remain available, and F5 requests stop.

The Variables view searches A2L metadata and switches between all candidates and the sampling set. It shows measurements and calibration characteristics; selecting a row does not read the ECU. Press Enter to open a modal with the full symbol name, capability reason, latest value/status/time, and supported actions. One-dimensional MEASUREMENT arrays expose 0-based single-element reads and pages of up to eight values. Fixed-axis CURVE symbols expose single-point reads/writes; unsupported axes or layouts remain read-only. Read/write actions use the same serialized operations and safety checks as their command equivalents. Esc closes the modal and discards the target draft while retaining the list query, selected symbol, and scroll position. While a modal is open, background views and the command input cannot receive events.

F2 cycles the three views; Tab changes focus; ↑/↓ browse command history when the command box is focused and select rows when the preview is focused; PgUp/PgDn scroll the current result/log panel using the available terminal height. Enter opens the selected variable's details; Esc exits the current modal step, or returns focus to the command box when no modal is open. F5 cancels any pending confirmation before requesting stop. The UI requires an interactive terminal; below 80×24 it keeps the command/status areas available but disables variable details and dangerous confirmations.

Writes still follow the same conservative validation, pre-read, default-cancel confirmation, write, and read-back path as the `write` command. The modal confirmation shows the full symbol and current/target values (or the `FREE_DAQ` consequence/profile). Only `Y/y` confirms; `N/n` or Esc cancels, Enter is not an acceptance, F5 cancels and requests stop, and other input is swallowed. A stop/cancel processed before dispatch revokes authorization; an XCP command already in flight cannot be undone. Ordinary commands are not queued while another is running; a rejected submit keeps its text in the input for retry, while `stop` and `quit` are prioritized. Asynchronous results with an outdated search revision are discarded, preserving the newer draft and current list; valid candidate-list updates are deferred while a modal is open and applied after it closes. Result/log buffers retain at most 512 lines; `list all` uses a separate complete snapshot.

## Safety and capability limits

A2L presence and the A2L `READ_WRITE` flag are metadata, not proof that a connected ECU permits access. Reads/writes are conservatively refused for unknown widths, bit fields, addresses outside the 32-bit XCP range, incompatible address granularity, or A2L/ECU byte-order disagreement. Scalar writes are limited to invertible `CHARACTERISTIC/VALUE`; fixed-axis `CHARACTERISTIC/CURVE` writes are limited to a validated single point. Both require explicit confirmation plus static A2L and runtime XCP page permission. MEASUREMENT arrays are read-only and only support one-dimensional indexed/page reads; they are not added to DAQ. ECU-side protection can still reject writes; the tool does not invent or automatically run a Seed&Key algorithm. The local command mutex cannot prevent the ECU or another XCP master from switching pages between the final permission check and WRITE; post-write checks can detect some changes but cannot undo a write that reached a different page.

DAQ is deliberately not advertised as compatible with arbitrary XCP slaves. Unknown/unverified DTO headers, timestamp modes, missing/ambiguous event declarations, insufficient resources, or mismatched runtime facts are rejected rather than decoded speculatively. The generic profile does not use XCPlite's proprietary DTO marker, first-ODT timestamp convention, or timestamp unit mapping. Arrays are manual-read only (no array DAQ); only verified fixed-axis CURVE single points are supported, not arbitrary curves, MAPs, structures, static PREDEFINED DAQ, page switching, or automatic ECU management. TCP/CAN remain outside this tool's supported scope.

Only connect to an ECU for which you have authorization. Use a non-critical calibration value for manual write testing and follow the ECU owner's procedure for restoring it. Automated tests do not constitute validation against every ECU; manual Slave validation is optional and was not part of the build/test suite.
