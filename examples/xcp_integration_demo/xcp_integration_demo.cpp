#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <variant>
#include <utility>
#include <vector>

#include <libxcp/a2l/a2l_bridge.hpp>
#include <libxcp/a2l/ia2l_database.hpp>
#include <libxcp/daq/dto_envelope_types.hpp>
#include <libxcp/measurement/measurement_database.hpp>
#include <libxcp/measurement/measurement_sample.hpp>
#include <libxcp/measurement/measurement_session.hpp>
#include <libxcp/protocol_types.hpp>
#include <libxcp/udp_transport.hpp>
#include <libxcp/udp_transport_config.hpp>
#include <libxcp/xcp_error.hpp>
#include <libxcp/xcp_master.hpp>

#include <a2l/a2l_measurement_database.hpp>
#include <a2l/measurement_runtime_profile.hpp>
#include "demo_process.hpp"

#ifndef CPP_DEMO_EXECUTABLE
#error CPP_DEMO_EXECUTABLE must be provided by CMake
#endif
#ifndef XCP_104_AML_FILE
#error XCP_104_AML_FILE must be provided by CMake
#endif

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace calmcar::xcp;

namespace {

struct Options {
    std::string host = "127.0.0.1";
    std::uint16_t port = 5555;
    fs::path a2l_path;
    fs::path uploaded_a2l_path = "uploaded.a2l";
    fs::path run_dir = "xcp_integration_demo_run";
    bool upload_a2l = false;
    bool xcplite_profile = false;
    bool cpp_demo_profile = false;
    std::vector<std::string> symbols;
    std::uint16_t event_channel = 0;
    bool event_channel_set = false;
    std::uint8_t event_cycle = 0;
    bool event_cycle_set = false;
    std::uint8_t event_unit = 0;
    bool event_unit_set = false;
    std::map<std::string, std::uint64_t> symbol_events;
    std::vector<A2lEventMetadata> event_metadata;
    IdentificationFieldType id_type = IdentificationFieldType::RelativeWord;
    bool id_type_set = false;
    std::size_t id_header_bytes = 0;
    std::uint64_t timestamp_unit_ns = 0;
    bool timestamp_first_odt_only = false;
    unsigned seconds = 5;
    std::optional<Address> write_address;
    AddressExtension write_extension = 0;
    Bytes write_value;
    std::optional<Bytes> expected_original;
    bool confirm_write = false;
    bool page_query = false;
    std::uint8_t segment = 0;
    std::uint8_t page = 0;
    std::optional<std::pair<std::uint8_t, std::uint8_t>> set_page;
    bool confirm_page_change = false;
    std::optional<std::uint8_t> freeze_segment;
    bool confirm_freeze = false;
    std::optional<CopyCalPageRequest> copy_page;
    bool confirm_page_copy = false;
    struct ModifyBitsSpec {
        Address address{0};
        std::uint8_t shift{0};
        std::uint16_t and_mask{0};
        std::uint16_t xor_mask{0};
    };
    std::optional<ModifyBitsSpec> modify_bits;
    bool confirm_modify_bits = false;
};

[[noreturn]] void UsageError(std::string_view message) {
    throw std::invalid_argument(std::string(message));
}

std::uint64_t ParseInteger(std::string_view text) {
    std::uint64_t result = 0;
    int base = 10;
    if (text.size() > 2 && text[0] == '0' &&
        (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
        base = 16;
    }
    const auto [ptr, ec] =
        std::from_chars(text.data(), text.data() + text.size(), result, base);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        UsageError("无效整数参数");
    }
    return result;
}

Bytes ParseHexBytes(std::string_view text) {
    Bytes bytes;
    std::string compact;
    for (char ch : text) {
        if (ch == ' ' || ch == ':' || ch == '-') continue;
        compact.push_back(ch);
    }
    if (compact.empty() || compact.size() % 2 != 0) {
        UsageError("HEX 字节串必须为非空偶数位十六进制");
    }
    for (std::size_t i = 0; i < compact.size(); i += 2) {
        unsigned value = 0;
        const auto [ptr, ec] = std::from_chars(
            compact.data() + i, compact.data() + i + 2, value, 16);
        if (ec != std::errc{} || ptr != compact.data() + i + 2 || value > 255) {
            UsageError("HEX 字节串含非法字符");
        }
        bytes.push_back(static_cast<std::uint8_t>(value));
    }
    return bytes;
}

std::vector<std::string> ParseNames(std::string_view text) {
    std::vector<std::string> result;
    std::stringstream stream{std::string(text)};
    std::string name;
    while (std::getline(stream, name, ',')) {
        if (name.empty()) UsageError("--symbols 不接受空名称");
        result.push_back(name);
    }
    if (result.empty()) UsageError("--symbols 至少需要一个符号");
    return result;
}

void ApplyCppDemoProfile(Options& o) {
    if (!o.cpp_demo_profile) return;
    o.host = "127.0.0.1";
    o.port = 5555;
    o.xcplite_profile = true;
    o.upload_a2l = true;
    o.a2l_path = o.run_dir / "uploaded.a2l";
    o.uploaded_a2l_path = o.a2l_path;
    if (o.symbols.empty()) {
        o.symbols = {"cpp_demo::temperature",    "cpp_demo::speed",
                     "cpp_demo::counter",        "cpp_demo::sum",
                     "cpp_demo::SigGen1.value_", "cpp_demo::SigGen2.value_"};
    }
    o.event_metadata = {{"SigGen1", "SigGen1", 0, 0, 0},
                        {"SigGen2", "SigGen2", 1, 0, 0},
                        {"mainloop", "mainloop", 2, 0, 0}};
    for (const auto& symbol : o.symbols) {
        std::uint64_t channel = 2;
        if (symbol.find("SigGen1") != std::string::npos)
            channel = 0;
        else if (symbol.find("SigGen2") != std::string::npos)
            channel = 1;
        else if (symbol.find("temperature") == std::string::npos &&
                 symbol.find("speed") == std::string::npos &&
                 symbol.find("counter") == std::string::npos &&
                 symbol.find("sum") == std::string::npos &&
                 symbol.find("loop_cycle") == std::string::npos &&
                 symbol.find("loop_histogram") == std::string::npos) {
            UsageError("cpp_demo profile 无此符号的已知事件绑定");
        }
        o.symbol_events[symbol] = channel;
    }
}

void PrintUsage() {
    std::puts(R"(XCP integration example (C++20)

Usage:
  xcp_integration_demo --profile cpp_demo [--run-dir DIR] [--seconds N]
  xcp_integration_demo --a2l FILE --symbols A,B --symbol-event A=CHANNEL
      --event-meta CHANNEL:CYCLE:UNIT --id-field absolute|relative-byte|relative-word
      --header-bytes N [--tick-ns N] [--timestamp-first-odt-only]
      [--host IPv4] [--port N] [--seconds N]

  --upload-a2l FILE       Fetch A2L through GET_ID+UPLOAD before measurement.
  --xcplite-profile       Derive the XCPlite DTO/timestamp profile at runtime.
  --write-address HEX --write-value HEX --confirm-write
                          Temporarily write 1..4 raw bytes, verify, then restore.
  --expected-original HEX Required pre-write raw-value guard for DOWNLOAD/MODIFY_BITS.
  --address-extension N   XCP address extension for calibration (default 0).
  --page-info SEG:PAGE    Read page/segment state only.
  --set-page SEG:PAGE --confirm-page-change
  --freeze-segment SEG --confirm-freeze
  --modify-bits ADDR:SHIFT:AND:XOR --confirm-modify-bits
  --copy-page S:P:S:P --confirm-page-copy (destructive; external backup required)
  --help                  Show this text.

Target ECU, A2L, event bindings, DTO envelope and timestamp units must be verified
for the actual Slave. Writes are disabled unless --confirm-write is present.
This sample does not implement a vendor Seed&Key algorithm.)");
}

Options ParseOptions(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        auto value = [&]() -> std::string_view {
            if (++i >= argc) UsageError("缺少选项值");
            return argv[i];
        };
        if (arg == "--help") {
            PrintUsage();
            std::exit(0);
        } else if (arg == "--host") {
            o.host = value();
        } else if (arg == "--port") {
            const auto v = ParseInteger(value());
            if (v == 0 || v > 65535) UsageError("UDP 端口超范围");
            o.port = static_cast<std::uint16_t>(v);
        } else if (arg == "--a2l") {
            o.a2l_path = value();
        } else if (arg == "--upload-a2l") {
            o.upload_a2l = true;
            o.uploaded_a2l_path = value();
        } else if (arg == "--xcplite-profile") {
            o.xcplite_profile = true;
        } else if (arg == "--profile") {
            const auto profile = value();
            if (profile != "cpp_demo")
                UsageError("唯一内置 profile 为 cpp_demo");
            o.cpp_demo_profile = true;
        } else if (arg == "--run-dir") {
            o.run_dir = value();
        } else if (arg == "--symbols") {
            o.symbols = ParseNames(value());
        } else if (arg == "--event-channel") {
            const auto v = ParseInteger(value());
            if (v > 65535) UsageError("event channel 超范围");
            o.event_channel = static_cast<std::uint16_t>(v);
            o.event_channel_set = true;
        } else if (arg == "--event-cycle") {
            const auto v = ParseInteger(value());
            if (v > 255) UsageError("event cycle 超范围");
            o.event_cycle = static_cast<std::uint8_t>(v);
            o.event_cycle_set = true;
        } else if (arg == "--event-unit") {
            const auto v = ParseInteger(value());
            if (v > 255) UsageError("event unit 超范围");
            o.event_unit = static_cast<std::uint8_t>(v);
            o.event_unit_set = true;
        } else if (arg == "--symbol-event") {
            const std::string spec(value());
            const auto eq = spec.find('=');
            if (eq == std::string::npos || eq == 0)
                UsageError("--symbol-event 格式 SYMBOL=CHANNEL");
            const std::uint64_t channel =
                ParseInteger(std::string_view(spec).substr(eq + 1));
            if (channel > UINT16_MAX) UsageError("event channel 超范围");
            o.symbol_events[spec.substr(0, eq)] = channel;
        } else if (arg == "--event-meta") {
            const std::string spec(value());
            std::vector<std::uint64_t> parts;
            std::size_t start = 0;
            for (;;) {
                const auto colon = spec.find(':', start);
                parts.push_back(ParseInteger(std::string_view(spec).substr(
                    start,
                    colon == std::string::npos ? colon : colon - start)));
                if (colon == std::string::npos) break;
                start = colon + 1;
            }
            if (parts.size() != 3 || parts[0] > UINT16_MAX ||
                parts[1] > UINT8_MAX || parts[2] > UINT8_MAX) {
                UsageError("--event-meta 格式 CHANNEL:CYCLE:UNIT");
            }
            const auto channel = static_cast<std::uint16_t>(parts[0]);
            o.event_metadata.push_back(
                A2lEventMetadata{"event_" + std::to_string(channel),
                                 "event_" + std::to_string(channel), channel,
                                 static_cast<std::uint8_t>(parts[1]),
                                 static_cast<std::uint8_t>(parts[2])});
        } else if (arg == "--id-field") {
            const auto v = value();
            if (v == "absolute")
                o.id_type = IdentificationFieldType::Absolute;
            else if (v == "relative-byte")
                o.id_type = IdentificationFieldType::RelativeByte;
            else if (v == "relative-word")
                o.id_type = IdentificationFieldType::RelativeWord;
            else
                UsageError(
                    "当前示例仅接受 absolute/relative-byte/relative-word");
            o.id_type_set = true;
        } else if (arg == "--header-bytes") {
            o.id_header_bytes = static_cast<std::size_t>(ParseInteger(value()));
        } else if (arg == "--tick-ns") {
            o.timestamp_unit_ns = ParseInteger(value());
        } else if (arg == "--timestamp-first-odt-only") {
            o.timestamp_first_odt_only = true;
        } else if (arg == "--seconds") {
            const auto v = ParseInteger(value());
            if (v == 0 || v > 3600) UsageError("采集时长必须在 1..3600 秒");
            o.seconds = static_cast<unsigned>(v);
        } else if (arg == "--write-address") {
            const auto v = ParseInteger(value());
            if (v > UINT32_MAX) UsageError("标定地址超出 32 位");
            o.write_address = static_cast<Address>(v);
        } else if (arg == "--write-value") {
            o.write_value = ParseHexBytes(value());
        } else if (arg == "--expected-original") {
            o.expected_original = ParseHexBytes(value());
        } else if (arg == "--address-extension") {
            const auto v = ParseInteger(value());
            if (v > 255) UsageError("地址扩展超范围");
            o.write_extension = static_cast<AddressExtension>(v);
        } else if (arg == "--confirm-write") {
            o.confirm_write = true;
        } else if (arg == "--page-info") {
            std::string spec(value());
            const auto colon = spec.find(':');
            if (colon == std::string::npos)
                UsageError("--page-info 格式为 SEG:PAGE");
            const auto seg =
                ParseInteger(std::string_view(spec).substr(0, colon));
            const auto page =
                ParseInteger(std::string_view(spec).substr(colon + 1));
            if (seg > 255 || page > 255) UsageError("segment/page 超范围");
            o.segment = static_cast<std::uint8_t>(seg);
            o.page = static_cast<std::uint8_t>(page);
            o.page_query = true;
        } else if (arg == "--set-page") {
            std::string spec(value());
            const auto colon = spec.find(':');
            if (colon == std::string::npos)
                UsageError("--set-page 格式为 SEG:PAGE");
            const auto seg =
                ParseInteger(std::string_view(spec).substr(0, colon));
            const auto page =
                ParseInteger(std::string_view(spec).substr(colon + 1));
            if (seg > 255 || page > 255) UsageError("segment/page 超范围");
            o.set_page = std::pair{static_cast<std::uint8_t>(seg),
                                   static_cast<std::uint8_t>(page)};
        } else if (arg == "--confirm-page-change") {
            o.confirm_page_change = true;
        } else if (arg == "--freeze-segment") {
            const auto seg = ParseInteger(value());
            if (seg > 255) UsageError("segment 超范围");
            o.freeze_segment = static_cast<std::uint8_t>(seg);
        } else if (arg == "--confirm-freeze") {
            o.confirm_freeze = true;
        } else if (arg == "--copy-page") {
            std::string spec(value());
            std::vector<std::uint64_t> parts;
            std::size_t start = 0;
            for (;;) {
                const auto colon = spec.find(':', start);
                parts.push_back(ParseInteger(std::string_view(spec).substr(
                    start,
                    colon == std::string::npos ? colon : colon - start)));
                if (colon == std::string::npos) break;
                start = colon + 1;
            }
            if (parts.size() != 4 ||
                std::any_of(parts.begin(), parts.end(),
                            [](auto v) { return v > 255; })) {
                UsageError(
                    "--copy-page 格式为 SRC_SEG:SRC_PAGE:DST_SEG:DST_PAGE");
            }
            o.copy_page =
                CopyCalPageRequest{static_cast<std::uint8_t>(parts[0]),
                                   static_cast<std::uint8_t>(parts[1]),
                                   static_cast<std::uint8_t>(parts[2]),
                                   static_cast<std::uint8_t>(parts[3])};
        } else if (arg == "--confirm-page-copy") {
            o.confirm_page_copy = true;
        } else if (arg == "--modify-bits") {
            std::string spec(value());
            std::vector<std::uint64_t> parts;
            std::size_t start = 0;
            for (;;) {
                const auto colon = spec.find(':', start);
                parts.push_back(ParseInteger(std::string_view(spec).substr(
                    start,
                    colon == std::string::npos ? colon : colon - start)));
                if (colon == std::string::npos) break;
                start = colon + 1;
            }
            if (parts.size() != 4 || parts[0] > UINT32_MAX || parts[1] > 16 ||
                parts[2] > UINT16_MAX || parts[3] > UINT16_MAX) {
                UsageError(
                    "--modify-bits 格式为 ADDRESS:SHIFT:AND_MASK:XOR_MASK");
            }
            o.modify_bits =
                Options::ModifyBitsSpec{static_cast<Address>(parts[0]),
                                        static_cast<std::uint8_t>(parts[1]),
                                        static_cast<std::uint16_t>(parts[2]),
                                        static_cast<std::uint16_t>(parts[3])};
        } else if (arg == "--confirm-modify-bits") {
            o.confirm_modify_bits = true;
        } else {
            UsageError("未知选项");
        }
    }

    ApplyCppDemoProfile(o);
    if (o.upload_a2l && o.a2l_path.empty()) o.a2l_path = o.uploaded_a2l_path;
    if (o.a2l_path.empty()) UsageError("必须提供 --a2l 或 --upload-a2l");
    if (o.event_channel_set && (o.event_cycle_set != o.event_unit_set)) {
        UsageError("默认事件绑定需要同时提供 --event-cycle 与 --event-unit");
    }
    if (o.event_channel_set && o.event_cycle_set) {
        o.event_metadata.push_back(
            A2lEventMetadata{"event_" + std::to_string(o.event_channel),
                             "event_" + std::to_string(o.event_channel),
                             o.event_channel, o.event_cycle, o.event_unit});
    }
    if (o.symbols.empty() && !o.page_query && !o.write_address && !o.set_page &&
        !o.freeze_segment && !o.copy_page && !o.modify_bits) {
        UsageError("至少指定测量符号或一项标定/查询操作");
    }
    if (o.set_page.has_value() != o.confirm_page_change) {
        UsageError("--set-page 必须与 --confirm-page-change 一起使用");
    }
    if (o.freeze_segment.has_value() != o.confirm_freeze) {
        UsageError("--freeze-segment 必须与 --confirm-freeze 一起使用");
    }
    if (o.copy_page.has_value() != o.confirm_page_copy) {
        UsageError("--copy-page 必须与 --confirm-page-copy 一起使用");
    }
    if (o.modify_bits.has_value() != o.confirm_modify_bits) {
        UsageError("--modify-bits 必须与 --confirm-modify-bits 一起使用");
    }
    if (!o.symbols.empty()) {
        if (!o.xcplite_profile && (!o.id_type_set || o.id_header_bytes == 0)) {
            UsageError("通用模式必须显式设置 --id-field 与 --header-bytes");
        }
        for (const auto& symbol : o.symbols) {
            if (!o.event_channel_set && !o.symbol_events.contains(symbol)) {
                UsageError(
                    "每个测量符号都必须有显式 --event-channel 或 "
                    "--symbol-event 绑定");
            }
        }
        if (o.xcplite_profile) {
            std::vector<std::uint16_t> bound_channels;
            for (const auto& symbol : o.symbols) {
                const auto it = o.symbol_events.find(symbol);
                const auto channel = static_cast<std::uint16_t>(
                    it != o.symbol_events.end() ? it->second : o.event_channel);
                if (std::find(bound_channels.begin(), bound_channels.end(),
                              channel) == bound_channels.end()) {
                    bound_channels.push_back(channel);
                }
            }
            for (auto channel : bound_channels) {
                const bool found = std::any_of(
                    o.event_metadata.begin(), o.event_metadata.end(),
                    [channel](const auto& event) {
                        return event.channel == channel;
                    });
                if (!found)
                    UsageError(
                        "XCPlite profile 需要每个通道的显式 --event-meta "
                        "CHANNEL:CYCLE:UNIT");
            }
        }
    }
    const unsigned mutating_operations =
        static_cast<unsigned>(o.write_address.has_value()) +
        static_cast<unsigned>(o.set_page.has_value()) +
        static_cast<unsigned>(o.freeze_segment.has_value()) +
        static_cast<unsigned>(o.copy_page.has_value()) +
        static_cast<unsigned>(o.modify_bits.has_value());
    if (mutating_operations > 1) {
        UsageError("每次运行只允许一个状态变更操作，避免多个恢复路径交叠");
    }
    if (o.write_address.has_value()) {
        if (!o.confirm_write)
            UsageError("写入默认关闭；显式增加 --confirm-write 才会执行");
        if (o.write_value.empty() || o.write_value.size() > 4) {
            UsageError("临时标量写入要求 1..4 个原始字节");
        }
        if (!o.expected_original ||
            o.expected_original->size() != o.write_value.size()) {
            UsageError("临时写入必须提供同长度 --expected-original allowlist");
        }
    } else if (o.modify_bits) {
        if (!o.expected_original || o.expected_original->size() != 4) {
            UsageError(
                "MODIFY_BITS 必须提供 4 字节 --expected-original allowlist");
        }
        if (!o.write_value.empty() || o.confirm_write) {
            UsageError(
                "--write-value/--confirm-write 只能与 --write-address "
                "一起使用");
        }
    } else if (!o.write_value.empty() || o.confirm_write ||
               o.expected_original) {
        UsageError(
            "写入相关选项必须与 --write-address 或 --modify-bits 一起使用");
    }
    return o;
}

std::unique_ptr<IXcpTransport> MakeTransport(const Options& o) {
    UdpTransportConfig config;
    config.remote_host = o.host;
    config.remote_port = o.port;
    config.local_host = "0.0.0.0";
    config.local_port = 0;
    return std::make_unique<UdpTransport>(std::move(config));
}

void PrintCapabilities(XcpMaster& master) {
    const SessionParameters session = master.GetSessionParameters();
    std::printf("CONNECT: MAX_CTO=%u MAX_DTO=%u CAL/PAG=%s\n",
                session.connect.max_cto, session.connect.max_dto,
                master.HasCalPagResource() ? "yes" : "no");
    if (const auto pag = master.QueryPagProcessorInfo()) {
        std::printf(
            "GET_PAG_PROCESSOR_INFO: max_segment=%u properties=0x%02x\n",
            pag->max_segment, static_cast<unsigned>(pag->properties));
    } else {
        std::puts("GET_PAG_PROCESSOR_INFO: unsupported/not declared");
    }
    if (const auto daq = master.QueryDaqProcessorInfo()) {
        std::printf("GET_DAQ_PROCESSOR_INFO: key=0x%02x properties=0x%02x\n",
                    daq->key_byte.raw, daq->properties);
    } else {
        std::puts("GET_DAQ_PROCESSOR_INFO: unsupported");
    }
}

void RunPageInfo(XcpMaster& master, std::uint8_t segment, std::uint8_t page) {
    if (!master.HasCalPagResource()) {
        std::puts("CAL/PAG unavailable; page query skipped");
        return;
    }
    const auto xcp_page = master.GetCalPage(CalPageAccessMode::Xcp, segment);
    const auto ecu_page = master.GetCalPage(CalPageAccessMode::Ecu, segment);
    const auto page_info = master.GetPageInfo(segment, page);
    const auto segment_info =
        master.GetSegmentInfo(SegmentInfoMode::StandardProperties, segment,
                              SegmentInfoSelector::SegmentAddress);
    const auto segment_mode = master.GetSegmentMode(segment);
    std::printf(
        "segment=%u XCP-page=%s ECU-page=%s requested-page=%u info=%s "
        "mode=%s\n",
        segment,
        xcp_page ? std::to_string(xcp_page->page).c_str() : "unsupported",
        ecu_page ? std::to_string(ecu_page->page).c_str() : "unsupported", page,
        page_info ? "available" : "unsupported",
        segment_mode ? "available" : "unsupported");
    if (segment_info)
        std::puts("GET_SEGMENT_INFO: standard properties available");
}

void RunSafePageSwitch(XcpMaster& master, std::uint8_t segment,
                       std::uint8_t target_page) {
    if (!master.HasCalPagResource())
        throw std::runtime_error("CAL/PAG resource absent");
    const auto prior = master.GetCalPage(CalPageAccessMode::Xcp, segment);
    if (!prior)
        throw std::runtime_error(
            "GET_CAL_PAGE(XCP) unsupported; no page switch");
    try {
        master.SetCalPage(CalPageModeBit::kXcp, segment, target_page);
    } catch (...) {
        // A command timeout can be ambiguous. Query first; restore only if the
        // requested page is actually observed, and never replay the set
        // command.
        try {
            const auto observed =
                master.GetCalPage(CalPageAccessMode::Xcp, segment);
            if (observed && observed->page == target_page &&
                target_page != prior->page) {
                master.SetCalPage(CalPageModeBit::kXcp, segment, prior->page);
            }
        } catch (...) {
        }
        throw;
    }
    const auto selected = master.GetCalPage(CalPageAccessMode::Xcp, segment);
    if (!selected || selected->page != target_page) {
        throw std::runtime_error(
            "page switch verification failed; manual recovery may be required");
    }
    master.SetCalPage(CalPageModeBit::kXcp, segment, prior->page);
    const auto restored = master.GetCalPage(CalPageAccessMode::Xcp, segment);
    if (!restored || restored->page != prior->page) {
        throw std::runtime_error("original calibration page was not restored");
    }
}

void RunFreezeRoundTrip(XcpMaster& master, std::uint8_t segment) {
    if (!master.HasCalPagResource())
        throw std::runtime_error("CAL/PAG resource absent");
    const auto prior = master.GetSegmentMode(segment);
    if (!prior) throw std::runtime_error("GET_SEGMENT_MODE unsupported");
    const bool was_frozen =
        HasSegmentMode(prior->mode, SegmentModeBit::kFreeze);
    if (was_frozen) {
        std::puts("Segment already frozen; no state change performed.");
        return;
    }
    master.SetSegmentFreeze(true, segment);
    const auto active = master.GetSegmentMode(segment);
    if (!active || !HasSegmentMode(active->mode, SegmentModeBit::kFreeze)) {
        throw std::runtime_error(
            "freeze state could not be verified; manual recovery may be "
            "required");
    }
    master.SetSegmentFreeze(false, segment);
    const auto restored = master.GetSegmentMode(segment);
    if (!restored || HasSegmentMode(restored->mode, SegmentModeBit::kFreeze)) {
        throw std::runtime_error("segment freeze state was not restored");
    }
}

void RunPageCopy(XcpMaster& master, const CopyCalPageRequest& request) {
    if (!master.HasCalPagResource())
        throw std::runtime_error("CAL/PAG resource absent");
    std::puts(
        "WARNING: COPY_CAL_PAGE mutates the destination page; ensure it is "
        "backed up externally.");
    master.CopyCalPage(request);
}

void RunModifyBitsRoundTrip(XcpMaster& master, Address address,
                            AddressExtension extension, std::uint8_t shift,
                            std::uint16_t and_mask, std::uint16_t xor_mask,
                            const Bytes& expected_original) {
    if (!master.HasCalPagResource())
        throw std::runtime_error("CAL/PAG resource absent");
    const Bytes original = master.ReadMemoryBytes(address, extension, 4);
    if (original != expected_original) {
        throw std::runtime_error("MODIFY_BITS 原值不匹配 allowlist，拒绝操作");
    }
    try {
        master.ModifyBits(address, extension, shift, and_mask, xor_mask);
    } catch (...) {
        // MODIFY_BITS may already have executed. Do not replay or guess a
        // restore.
        std::fprintf(stderr,
                     "MODIFY_BITS outcome unknown; inspect the target before "
                     "any further write.\n");
        throw;
    }
    const Bytes changed = master.ReadMemoryBytes(address, extension, 4);
    if (changed == original) {
        throw std::runtime_error(
            "MODIFY_BITS readback did not change; original retained");
    }
    master.WriteMemoryBytes(address, extension, original);
    if (master.ReadMemoryBytes(address, extension, 4) != original) {
        throw std::runtime_error(
            "MODIFY_BITS demo could not restore original DWORD");
    }
}

void UnlockWithInjectedSeedKey(XcpMaster& master,
                               const SeedKeyCalculator& vendor_calculator) {
    if (!vendor_calculator) {
        throw std::invalid_argument("Seed&Key provider is not configured");
    }
    (void)master.Unlock(Resource::CalPag, vendor_calculator);
}

bool BytesEqual(const Bytes& lhs, const Bytes& rhs) { return lhs == rhs; }

void RunTemporaryWrite(XcpMaster& master, const Options& o) {
    if (!o.write_address) return;
    const Address address = *o.write_address;
    const Bytes original =
        master.ReadMemoryBytes(address, o.write_extension,
                               static_cast<ByteCount>(o.write_value.size()));
    if (o.expected_original && !BytesEqual(original, *o.expected_original)) {
        throw std::runtime_error("原值不匹配 allowlist，拒绝写入");
    }
    std::puts(
        "Calibration: target is explicitly opted in; raw original value backed "
        "up.");
    try {
        master.WriteMemoryBytes(address, o.write_extension, o.write_value);
    } catch (...) {
        // A timeout may mean the mutator executed. Never replay it blindly.
        try {
            const Bytes observed = master.ReadMemoryBytes(
                address, o.write_extension,
                static_cast<ByteCount>(o.write_value.size()));
            if (BytesEqual(observed, original)) {
                std::puts(
                    "Write outcome: original value remains; no retry "
                    "performed.");
            } else if (BytesEqual(observed, o.write_value)) {
                std::puts(
                    "Write outcome: new value observed; restoring backed-up "
                    "bytes once.");
                master.WriteMemoryBytes(address, o.write_extension, original);
                if (!BytesEqual(master.ReadMemoryBytes(
                                    address, o.write_extension,
                                    static_cast<ByteCount>(original.size())),
                                original)) {
                    throw std::runtime_error("恢复后读回不匹配");
                }
            } else {
                throw std::runtime_error(
                    "写入结果不确定且与原值/目标值均不匹配；未重放写命令");
            }
        } catch (const std::exception& recovery_error) {
            std::fprintf(stderr, "恢复/状态查询失败：%s\n",
                         recovery_error.what());
        }
        throw;
    }

    const Bytes readback =
        master.ReadMemoryBytes(address, o.write_extension,
                               static_cast<ByteCount>(o.write_value.size()));
    if (!BytesEqual(readback, o.write_value)) {
        if (BytesEqual(readback, original)) {
            throw std::runtime_error("写后读回仍为原值；未重试写命令");
        }
        throw std::runtime_error(
            "写后读回既非目标值也非原值；结果不确定，未盲目重放/恢复");
    }
    master.WriteMemoryBytes(address, o.write_extension, original);
    const Bytes restored = master.ReadMemoryBytes(
        address, o.write_extension, static_cast<ByteCount>(original.size()));
    if (!BytesEqual(restored, original)) {
        throw std::runtime_error("原始值恢复后读回不匹配");
    }
    std::puts("Calibration: write/readback/restore/readback all passed.");
}

void PrintMeasurementValue(const MeasurementValue& value) {
    std::visit(
        [](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, std::string>) {
                std::printf("\"%s\"", item.c_str());
            } else if constexpr (std::is_same_v<T, bool>) {
                std::printf("%s", item ? "true" : "false");
            } else if constexpr (std::is_same_v<T, double>) {
                std::printf("%.9g", item);
            } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                std::printf("%llu", static_cast<unsigned long long>(item));
            } else {
                std::printf("%lld", static_cast<long long>(item));
            }
        },
        value);
}

void StartLocalCppDemo(const Options& o, xcp_example::DemoProcess& process) {
    std::error_code ec;
    fs::create_directories(o.run_dir, ec);
    if (ec)
        throw std::runtime_error("创建 cpp_demo run-dir 失败：" + ec.message());
    fs::copy_file(XCP_104_AML_FILE, o.run_dir / "XCP_104.aml",
                  fs::copy_options::skip_existing, ec);
    if (ec) throw std::runtime_error("准备 XCP_104.aml 失败：" + ec.message());
    std::string error;
    if (!process.Start(CPP_DEMO_EXECUTABLE, o.run_dir, &error)) {
        throw std::runtime_error("启动 cpp_demo 失败：" + error);
    }
}

std::unique_ptr<XcpMaster> ConnectWithRetry(
    const Options& o, IEventListener* listener = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    std::string last_error;
    while (std::chrono::steady_clock::now() < deadline) {
        auto master = std::make_unique<XcpMaster>(MakeTransport(o),
                                                  CommandTimeouts{}, listener);
        try {
            master->Connect();
            return master;
        } catch (const std::exception& error) {
            last_error = error.what();
        }
        std::this_thread::sleep_for(200ms);
    }
    throw std::runtime_error("等待 XCP Slave CONNECT 超时：" + last_error);
}

Bytes FetchAndSaveA2l(const Options& o) {
    auto bootstrap = ConnectWithRetry(o);
    Bytes a2l_bytes = bootstrap->FetchA2lViaUpload();
    bootstrap->Disconnect();
    std::ofstream output(o.uploaded_a2l_path, std::ios::binary);
    if (!output) throw std::runtime_error("无法创建上传 A2L 文件");
    output.write(reinterpret_cast<const char*>(a2l_bytes.data()),
                 static_cast<std::streamsize>(a2l_bytes.size()));
    if (!output) throw std::runtime_error("写入上传 A2L 文件失败");
    return a2l_bytes;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = ParseOptions(argc, argv);
        xcp_example::DemoProcess cpp_demo_process;
        if (options.cpp_demo_profile) {
            StartLocalCppDemo(options, cpp_demo_process);
        }
        if (options.upload_a2l) {
            (void)FetchAndSaveA2l(options);
            if (options.cpp_demo_profile) {
                // The vendored demo is finalized for the A2L upload session;
                // restart the process before creating the measurement
                // session/master.
                cpp_demo_process.Stop();
                StartLocalCppDemo(options, cpp_demo_process);
            }
        }
        auto bridge_result = a2l::A2lBridge::Load(
            options.a2l_path.string(),
            a2l::LoadOptions{.require_if_data_xcp = false});
        if (!bridge_result.HasValue()) {
            throw std::runtime_error("A2L 加载失败：" +
                                     bridge_result.ErrorInfo().message);
        }
        auto bridge = std::move(bridge_result).Value();
        if (bridge->Database() == nullptr) {
            throw std::runtime_error("A2L 数据库尚未就绪");
        }
        A2lMeasurementDatabase a2l_database(*bridge->Database());

        // EventBound DB and session must outlive XcpMaster; create them before
        // it.
        MeasurementEventBindingCandidates bindings;
        for (const auto& symbol : options.symbols) {
            const auto it = options.symbol_events.find(symbol);
            const auto channel =
                it != options.symbol_events.end()
                    ? it->second
                    : static_cast<std::uint64_t>(options.event_channel);
            bindings[symbol] = {channel};
        }
        EventBoundMeasurementDatabase bound_database(a2l_database,
                                                     std::move(bindings));
        MeasurementSession session(bound_database);
        auto master = ConnectWithRetry(options, &session);
        session.Bind(*master);
        PrintCapabilities(*master);

        if (options.page_query) {
            RunPageInfo(*master, options.segment, options.page);
        }
        if (options.set_page) {
            RunSafePageSwitch(*master, options.set_page->first,
                              options.set_page->second);
        }
        if (options.freeze_segment) {
            RunFreezeRoundTrip(*master, *options.freeze_segment);
        }
        if (options.copy_page) RunPageCopy(*master, *options.copy_page);
        if (options.modify_bits) {
            const auto& op = *options.modify_bits;
            RunModifyBitsRoundTrip(*master, op.address, options.write_extension,
                                   op.shift, op.and_mask, op.xor_mask,
                                   *options.expected_original);
        }
        RunTemporaryWrite(*master, options);

        if (!options.symbols.empty()) {
            XcpliteMeasurementRuntimeProfile xcplite_profile;
            if (options.xcplite_profile) {
                auto profile_result = BuildXcpliteMeasurementRuntimeProfile(
                    *master, options.event_metadata);
                if (!profile_result.HasValue()) {
                    throw std::runtime_error(
                        "XCPlite profile 不匹配：" +
                        profile_result.ErrorInfo().message);
                }
                xcplite_profile = profile_result.Value();
                ApplyMeasurementRuntimeProfile(session, xcplite_profile);
            } else {
                session.SetEnvelopeMode(options.id_type,
                                        options.id_header_bytes);
                session.SetTimestampFirstOdtOnly(
                    options.timestamp_first_odt_only);
                session.SetTimestampUnit(options.timestamp_unit_ns);
            }
            for (const auto& symbol : options.symbols) session.Add(symbol);
            session.Prepare();
            std::atomic<std::uint64_t> frames{0};
            session.Start([&](const MeasurementFrame& frame) {
                ++frames;
                for (const auto& sample : frame.samples) {
                    std::printf(
                        "DAQ=%u ODT=%u t=%s%llu %s raw=", frame.daq_list,
                        frame.odt, frame.timestamp_valid ? "ns:" : "raw:",
                        static_cast<unsigned long long>(
                            frame.timestamp_valid ? static_cast<std::uint64_t>(
                                                        frame.timestamp.count())
                                                  : frame.timestamp_raw),
                        sample.name.c_str());
                    for (std::uint8_t byte : sample.raw)
                        std::printf("%02x", byte);
                    std::printf(" physical=");
                    if (sample.valid)
                        PrintMeasurementValue(sample.value);
                    else
                        std::printf("<invalid>");
                    std::printf(" valid=%s\n", sample.valid ? "yes" : "no");
                }
            });
            std::this_thread::sleep_for(std::chrono::seconds(options.seconds));
            session.Stop();
            const auto stats = session.Statistics();
            std::printf(
                "DAQ summary: frames=%llu received=%llu dropped=%llu "
                "decode_errors=%llu timestamp_wraps=%llu\n",
                static_cast<unsigned long long>(frames.load()),
                static_cast<unsigned long long>(stats.dto_received),
                static_cast<unsigned long long>(stats.dto_dropped),
                static_cast<unsigned long long>(stats.decode_errors),
                static_cast<unsigned long long>(stats.timestamp_wraps));
        }
        master->Disconnect();
        return 0;
    } catch (const XcpException& error) {
        std::fprintf(stderr, "XCP error: %s\n", error.what());
        return 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Error: %s\n", error.what());
        return 2;
    }
}
