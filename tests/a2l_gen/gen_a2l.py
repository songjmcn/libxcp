#!/usr/bin/env python3
# =============================================================================
# gen_a2l.py —— A2L 黄金样本生成器（R4 §7.1-A；批次10 重构：SPEC 外置 JSON）
#
# 定位：产出「a2llib（main HEAD）能解析、且期望值由 spec 独立算出」的输入文件。
# 语法逐条对照 thirdparty/a2llib 的 bison/flex 源（只读参考，不修改上游）：
#   1) ASAP2 块一律 `/begin KEY ... /end KEY`（闭合关键字重复）；
#   2) `/begin IF_DATA XCP/XCPplus … /end IF_DATA` 由主干词法器 ReadIfData()
#      整段吞入直到 `/endIF_DATA`（跳空白匹配），可多行；
#   3) XCP 子词法器 0x… 返回 UINT；PROTOCOL_LAYER 的 ADDRESS_GRANULARITY
#      ident 仅占位（上游不调用 SetAddressGranularity，恒默认 BYTE=1）；
#   4) MEASUREMENT 必填 8 定位参数；DAQ_LIST 必须嵌套在 DAQ 块内；
#      UDP 主机关键字 HOST_NAME；FNC_VALUES 形如 `FNC_VALUES 0 UWORD ROW_DIR
#      DIRECT`（position datatype index_mode address_type，全为定位参数）；
#      FORMULA 块允许空属性（formula_attribute %empty）；
#      主干词法器大小写敏感（已实测无 caseless 选项）→ 大小写混用属语法错误，
#      作为负例静态样本而非"合法畸形"变体。
#   5) 批次12 实测：XCPonUDP/IP 选项为可重复列表（`udp_ip_options: %empty |
#      udp_ip_options udp_ip_option`，顺序自由，但 `transport_layer_instance`
#      必须在选项之后）；`PACKET_ALIGNMENT <IDENT>` 与
#      `OPTIONAL_TL_SUBCMD <IDENT>` 的 IDENT 由 XcpOnUdpIp 名称表逐项匹配，
#      **未命中时上游静默保持默认值/静默丢弃**（不报语法错），故本生成器
#      对超范围取值主动报错（见 UDP_ALIGNMENTS / UDP_SUBCMDS）。
#
# 用法：python gen_a2l.py --out <目录> [--spec <json>]
#   默认扫描本目录 golden_spec*.json 全部生成（CTest fixture 一次产全）。
#   --variants 额外产基本样本的行尾/空白/UTF-8-BOM 变体（A-6 缓解①）。
#
# 期望值独立推导：样本报文的 sample_raw_int/sample_phys 由 COMPU 系数反算，
# 不经 a2llib；C++ 侧以常量固化同一组期望值。
# =============================================================================

import argparse
import json
from pathlib import Path

# ---------------------------------------------------------------------------
# 数字字面量：0x 十六进制（spec 里既可写 int 也可写 "0x.." 字符串）
# ---------------------------------------------------------------------------
def fmt_int(v) -> str:
    """把 int 或 '0x..' 字符串格式化为 A2L 十六进制字面量。"""
    if isinstance(v, str):
        return v if v.lower().startswith("0x") else f"0x{int(v):X}"
    return f"0x{int(v):X}"


def fmt_num(v) -> str:
    """数字 → C++ 字面量（float 保留 .0 / 科学计数）。"""
    if isinstance(v, float) and v.is_integer():
        return f"{v:.1f}"
    return repr(v) if isinstance(v, float) else str(v)


# ---------------------------------------------------------------------------
# 块发射器
# ---------------------------------------------------------------------------
def emit_record_layouts(mod):
    """RECORD_LAYOUT：属性串原样输出（含 FNC_VALUES 等定位参数）。"""
    lines = []
    for rl in mod.get("record_layouts", []):
        lines.append(f'    /begin RECORD_LAYOUT {rl["name"]}')
        for attr in rl.get("attributes", []):
            lines.append(f'      {attr}')
        lines.append('    /end RECORD_LAYOUT')
    if lines:
        lines.append('')
    return lines


def emit_compu_methods(mod):
    """COMPU_METHOD / COMPU_TAB / COMPU_VTAB（类型名与系数按 spec 原样）。"""
    lines = []
    for t in mod.get("compu_tabs", []):
        lines.append(
            f'    /begin COMPU_TAB {t["name"]} "{t.get("desc", "")}" '
            f'{t["type"]} {len(t["pairs"])}')
        for a, b in t["pairs"]:
            lines.append(f'      {fmt_num(a)} {fmt_num(b)}')
        lines.append('    /end COMPU_TAB')
    for t in mod.get("compu_vtabs", []):
        lines.append(
            f'    /begin COMPU_VTAB {t["name"]} "{t.get("desc", "")}" '
            f'{t["type"]} {len(t["pairs"])}')
        for a, b in t["pairs"]:
            lines.append(f'      {fmt_num(a)} "{b}"')
        lines.append('    /end COMPU_VTAB')
    for cm in mod.get("compu_methods", []):
        lines.append(
            f'    /begin COMPU_METHOD {cm["name"]} "{cm.get("desc", "")}" '
            f'{cm["type"]} "{cm.get("format", "%6.2")}" "{cm.get("unit", "")}"')
        if "coeffs_linear" in cm:
            o, f = cm["coeffs_linear"]
            lines.append(f'      COEFFS_LINEAR {fmt_num(o)} {fmt_num(f)}')
        elif "coeffs" in cm:
            lines.append('      COEFFS ' + ' '.join(fmt_num(c) for c in cm["coeffs"]))
        elif "tab_ref" in cm:
            lines.append(f'      COMPU_TAB_REF {cm["tab_ref"]}')
        if cm.get("ref_unit"):
            # B-10 单位回退：符号无 PHYS_UNIT 时 ConversionInfo.unit 取它
            lines.append(f'      REF_UNIT {cm["ref_unit"]}')
        if cm.get("formula"):
            # formula_attribute 允许空 → 文本段直接 /end（a2lparser.y:774-780）
            lines.append(f'      /begin FORMULA "{cm["formula"]}" /end FORMULA')
        lines.append('    /end COMPU_METHOD')
    if lines:
        lines.append('')
    return lines


def emit_measurements(mod):
    """MEASUREMENT：8 定位参数 + 可选属性（扩展/位掩码/维度/布局/单位）。"""
    lines = []
    for m in mod.get("measurements", []):
        lines.append(
            f'    /begin MEASUREMENT {m["name"]} "{m.get("desc", "")}" '
            f'{m["datatype"]} {m.get("compu", "CM_IDENT")} 0 0 '
            f'{m.get("lower", 0)} {m.get("upper", 65535)}')
        lines.append('      BYTE_ORDER MSB_LAST')
        lines.append(f'      ECU_ADDRESS {fmt_int(m["address"])}')
        lines.append(f'      ECU_ADDRESS_EXTENSION {fmt_int(m.get("ext", 0))}')
        if m.get("array_size"):
            lines.append(f'      ARRAY_SIZE {m["array_size"]}')
        if m.get("matrix_dim"):
            lines.append(
                '      MATRIX_DIM ' + ' '.join(str(d) for d in m["matrix_dim"]))
        if m.get("layout"):
            lines.append(f'      LAYOUT {m["layout"]}')
        if m.get("bit_mask") is not None:
            lines.append(f'      BIT_MASK {fmt_int(m["bit_mask"])}')
        if m.get("read_write", True):
            lines.append('      READ_WRITE')
        if m.get("phys_unit"):
            lines.append(f'      PHYS_UNIT "{m["phys_unit"]}"')
        lines.append('    /end MEASUREMENT')
    return lines


def emit_characteristics(mod):
    """CHARACTERISTIC：name desc TYPE address deposit maxdiff compu lo hi。"""
    lines = []
    for c in mod.get("characteristics", []):
        lines.append(
            f'    /begin CHARACTERISTIC {c["name"]} "{c.get("desc", "")}" '
            f'{c["ctype"]} {fmt_int(c["address"])} {c["deposit"]} 0.0 '
            f'{c.get("compu", "CM_IDENT")} 0 {c.get("upper", 10000)}')
        lines.append('      BYTE_ORDER MSB_LAST')
        lines.append(f'      ECU_ADDRESS_EXTENSION {fmt_int(c.get("ext", 0))}')
        lines.append('    /end CHARACTERISTIC')
    return lines


# ---------------------------------------------------------------------------
# XCPonUDP/IP 选项合法取值（批次12）——逐条取自上游源码，不是规范推测：
#   PACKET_ALIGNMENT 的 IDENT 由 XcpOnUdpIp::SetPacketAlignment 查表
#   （src/xcp/xcponudpip.cpp："PACKET_ALIGNMENT_8/16/32"），未命中即静默保持默认；
#   OPTIONAL_TL_SUBCMD 的 IDENT 由 XcpOnUdpIp::AddSubCmd 查表，码值 = 0xFA+index，
#   表内空槽（0xFB/0xFE）不可用，故只列 4 个实际可识别名。
# ---------------------------------------------------------------------------
UDP_ALIGNMENTS = {"PACKET_ALIGNMENT_8", "PACKET_ALIGNMENT_16",
                  "PACKET_ALIGNMENT_32"}
UDP_SUBCMDS = {"GET_DAQ_CLOCK_MULTICAST", "SET_SLAVE_IP_ADDRESS",
               "GET_SLAVE_ID_EXTENDED", "GET_SLAVE_ID"}


def emit_if_data(block):
    """单个 IF_DATA 块（XCP 或 XCPplus；协议名决定子文法首 token）。"""
    proto = block.get("protocol", "XCP")
    lines = []
    header = f'    /begin IF_DATA {proto}'
    if proto == "XCPplus":
        header += f' {block.get("xcpplus_version", 11)}'
    lines.append(header)
    pl = block.get("protocol_layer", {})
    timers = pl.get("timers", [1, 1, 5, 5, 5, 1, 1])
    lines.append(
        f'      /begin PROTOCOL_LAYER {fmt_int(pl.get("version", "0x100"))} '
        + ' '.join(str(t) for t in timers) + ' '
        f'{fmt_int(pl.get("max_cto", "0x10"))} {fmt_int(pl.get("max_dto", "0x20"))} '
        'BYTE_ORDER_MSB_LAST ADDRESS_GRANULARITY_BYTE /end PROTOCOL_LAYER')
    daq = block.get("daq")
    if daq:
        dtype = daq.get("type", "STATIC")
        lines.append(
            f'      /begin DAQ {dtype} {daq.get("max_daq", 3)} '
            f'{daq.get("max_event", 2)} {daq.get("min_daq", 0)} '
            f'{daq.get("optimisation", "OPTIMISATION_TYPE_DEFAULT")} '
            f'{daq.get("addr_ext", "ADDRESS_EXTENSION_DAQ")} '
            f'{daq.get("id_field", "IDENTIFICATION_FIELD_TYPE_ABSOLUTE")} '
            f'{daq.get("granularity", "GRANULARITY_ODT_ENTRY_SIZE_DAQ_BYTE")} '
            f'{daq.get("max_odt_entry_size", 4)} '
            f'{daq.get("overload", "OVERLOAD_INDICATION_EVENT")}')
        for ev in daq.get("events", []):
            lines.append(
                f'        /begin EVENT {ev["name"]} "{ev.get("short", ev["name"])}" '
                f'{ev.get("number", 1)} {ev.get("type", "DAQ")} '
                f'{ev.get("max_daq_list", 1)} {ev.get("time_cycle", 2)} '
                f'{ev.get("time_unit", 3)} {ev.get("priority", 1)} /end EVENT')
        dl = daq.get("daq_list")
        if dl:
            lines.append(f'        /begin DAQ_LIST {dl["number"]}')
            lines.append(f'          DAQ_LIST_TYPE {dl.get("type", "DAQ")}')
            lines.append(f'          MAX_ODT {dl.get("max_odt", 1)}')
            lines.append(f'          MAX_ODT_ENTRIES {dl.get("max_odt_entries", 2)}')
            lines.append(f'          FIRST_PID {dl.get("first_pid", 1)}')
            if "event_fixed" in dl:
                lines.append(f'          EVENT_FIXED {dl["event_fixed"]}')
            lines.append('          /begin PREDEFINED')
            lines.append(f'            /begin ODT {dl.get("odt_number", 1)}')
            for e in dl["entries"]:
                lines.append(
                    f'              ODT_ENTRY {e["number"]} {fmt_int(e["address"])} '
                    f'{fmt_int(e.get("ext", 0))} {e["size"]} '
                    f'{e.get("bit_offset", 0)}')
            lines.append('            /end ODT')
            lines.append('          /end PREDEFINED')
            lines.append('        /end DAQ_LIST')
        lines.append('      /end DAQ')
    udp = block.get("udp")
    if udp:
        lines.append(
            f'      /begin XCP_ON_UDP_IP {fmt_int(pl.get("version", "0x100"))} '
            f'{fmt_int(udp["port"])}')
        lines.append(f'        HOST_NAME "{udp.get("host", "localhost")}"')
        # 批次12：PACKET_ALIGNMENT / OPTIONAL_TL_SUBCMD 的取值必须是上游
        # 词法器认得的 IDENT 字面量（xcpdataflexer.l 表 + XcpOnUdpIp 名称表）。
        # 超范围值一律报错而非静默跳过——上游对未识别值是**静默降级**
        # （alignment 保持默认 8、subcmd 直接丢弃），若这里也静默就测不到差异。
        align = udp.get("packet_alignment")
        if align is not None:
            if align not in UDP_ALIGNMENTS:
                raise SystemExit(
                    f'非法 packet_alignment: {align!r}（仅接受 '
                    f'{", ".join(sorted(UDP_ALIGNMENTS))}）')
            lines.append(f'        PACKET_ALIGNMENT {align}')
        for cmd in udp.get("sub_commands", []):
            if cmd not in UDP_SUBCMDS:
                raise SystemExit(
                    f'非法 sub_command: {cmd!r}（仅接受 '
                    f'{", ".join(sorted(UDP_SUBCMDS))}）')
            lines.append(f'        OPTIONAL_TL_SUBCMD {cmd}')
        lines.append('      /end XCP_ON_UDP_IP')
    lines.append('    /end IF_DATA')
    return lines


def build_module(mod):
    """一个 MODULE 的全部内容（含可选 IF_DATA 块列表）。"""
    lines = []
    lines.append(f'  /begin MODULE {mod["name"]} "{mod.get("comment", "")}"')
    lines.append('')
    if mod.get("mod_common", True):
        lines += [
            '    /begin MOD_COMMON "smoke"',
            '      BYTE_ORDER MSB_LAST',
            '      DEPOSIT ABSOLUTE',
            '      ALIGNMENT_BYTE 1',
            '      ALIGNMENT_WORD 1',
            '      ALIGNMENT_LONG 1',
            '    /end MOD_COMMON',
            '',
        ]
    if mod.get("mod_par", True):
        lines += [
            '    /begin MOD_PAR "smoke par"',
            '      CPU_TYPE "SMOKE-CPU"',
            '    /end MOD_PAR',
            '',
        ]
    lines += emit_record_layouts(mod)
    lines += emit_compu_methods(mod)
    lines += emit_measurements(mod)
    lines += emit_characteristics(mod)
    for block in mod.get("if_data", []):
        lines += emit_if_data(block)
        lines.append('')
    lines.append(f'  /end MODULE {mod["name"]}'.replace(
        f'/end MODULE {mod["name"]}', '/end MODULE'))
    return lines


def build_a2l(spec):
    """整文件：ASAP2_VERSION + PROJECT + 全部 MODULE。"""
    lines = ['ASAP2_VERSION 1 61', '']
    lines.append(
        f'/begin PROJECT {spec["project"]} '
        f'"{spec.get("project_desc", "libxcp A2L golden sample")}"')
    lines.append('')
    for mod in spec["modules"]:
        lines += build_module(mod)
    lines.append('/end PROJECT')
    return lines


# ---------------------------------------------------------------------------
# 畸形但合法变体（A-6 缓解①；只做已实测上游接受的变换）
# ---------------------------------------------------------------------------
def variant_lf(text: str) -> str:
    """CRLF → LF（词法器对行尾不敏感）。"""
    return text.replace("\r\n", "\n")


def variant_ws(text: str) -> str:
    """字符串外的单空格 → 三空格（拉宽空白；不动字符串内文字）。"""
    out = []
    in_str = False
    for ch in text:
        if ch == '"':
            in_str = not in_str
        elif ch == ' ' and not in_str:
            out.append('   ')
            continue
        out.append(ch)
    return ''.join(out)


def variant_bom(text: str) -> str:
    """UTF-8 BOM + LF 行尾（上游 CheckBom 路径）。"""
    return '\ufeff' + variant_lf(text)


def build_expected(spec):
    """由 spec 独立反算期望值（raw↔phys、维度、DTO 帧），不经 a2llib。"""
    mod = spec["modules"][0]
    methods = {cm["name"]: cm for cm in mod.get("compu_methods", [])}
    symbols = {}
    for m in mod.get("measurements", []):
        size = {"UBYTE": 1, "SBYTE": 1, "UWORD": 2, "SWORD": 2, "ULONG": 4,
                "SLONG": 4, "A_UINT64": 8, "A_INT64": 8,
                "FLOAT16_IEEE": 2, "FLOAT32_IEEE": 4,
                "FLOAT64_IEEE": 8}[m["datatype"]]
        item = {
            "module": mod["name"],
            "qualified": f'{mod["name"]}::{m["name"]}',
            "datatype": m["datatype"],
            "element_size_bytes": size,
            "xcp_address": int(m["address"], 0) if isinstance(m["address"], str)
                           else m["address"],
            "address_extension": int(m.get("ext", 0), 0)
                if isinstance(m.get("ext", 0), str) else m.get("ext", 0),
            "read_write": m.get("read_write", True),
        }
        if m.get("array_size"):
            item["dimensions"] = [{"extent": m["array_size"], "byte_stride": size}]
            item["byte_size"] = m["array_size"] * size
        # 样本反算：由 COMPU 系数独立算出（经 double 都精确的取值才列入）
        if "sample_raw_int" in m:
            raw = m["sample_raw_int"]
            cm = methods.get(m.get("compu", "CM_IDENT"))
            item["sample_raw_int"] = raw
            if cm and cm["type"] == "LINEAR":
                o, f = cm["coeffs_linear"]
                item["sample_phys"] = (0.0 + raw * f) + raw * o
            else:
                item["sample_phys"] = raw
        symbols[m["name"]] = item
    result = {"symbols": symbols, "modules": [x["name"] for x in spec["modules"]]}
    daq = spec["modules"][0].get("if_data", [{}])[0].get("daq")
    if daq and daq.get("daq_list"):
        dl = daq["daq_list"]
        result["daq_list"] = {
            "number": dl["number"],
            "entries": [{"address": e["address"], "ext": e.get("ext", 0),
                         "size": e["size"]} for e in dl["entries"]],
        }
    return result


def main():
    parser = argparse.ArgumentParser(description="生成 A2L 黄金样本")
    parser.add_argument("--out", required=True, help="输出目录")
    parser.add_argument("--spec", default=None,
                        help="指定单个 spec JSON（默认扫本目录 golden_spec*.json）")
    parser.add_argument("--variants", action="store_true",
                        help="额外生成基本样本的 lf/ws/bom 变体")
    args = parser.parse_args()

    script_dir = Path(__file__).resolve().parent
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.spec:
        specs = [Path(args.spec)]
    else:
        specs = sorted(script_dir.glob("golden_spec*.json"))
    if not specs:
        raise SystemExit("未找到 golden_spec*.json")

    written = []
    for spec_path in specs:
        spec = json.loads(spec_path.read_text(encoding="utf-8"))
        # 输出名 = spec 文件名去掉 _spec（golden_spec_xcpplus → golden_xcpplus）
        stem = spec_path.stem.replace("_spec", "")
        text = "\r\n".join(build_a2l(spec)) + "\r\n"
        # 非纯 ASCII spec（含中文描述）必须 UTF-8；纯 ASCII 也统一 UTF-8（ASCII 兼容）
        a2l_path = out_dir / f"{stem}.a2l"
        a2l_path.write_text(text, encoding="utf-8", newline="")
        written.append(a2l_path)
        # 基本样本附带期望值（供人核对；C++ 侧以常量固化同一组值）
        if stem == "golden_basic":
            exp = out_dir / "expected.json"
            exp.write_text(json.dumps(build_expected(spec), ensure_ascii=False,
                                      indent=2), encoding="utf-8")
            written.append(exp)
        if args.variants and stem == "golden_basic":
            for name, fn in (("lf", variant_lf), ("ws", variant_ws),
                             ("bom", variant_bom)):
                vp = out_dir / f"golden_basic_{name}.a2l"
                vp.write_text(fn(text), encoding="utf-8", newline="")
                written.append(vp)

    for p in written:
        print(f"generated: {p}")


if __name__ == "__main__":
    main()
