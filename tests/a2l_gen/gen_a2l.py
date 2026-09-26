#!/usr/bin/env python3
# =============================================================================
# gen_a2l.py —— 最小 A2L 黄金样本生成器（code-plan/A2L_CMake最小开发验证集_R4.md §5）
#
# 定位：产出「a2llib（main HEAD）能解析、且期望值由 spec 独立算出」的输入文件。
# 语法逐条对照 thirdparty/a2llib 的 bison/flex 源（只读参考，不修改上游）：
#   - 主干：src/a2lparser.y + src/a2lflexer.l + src/a2lscanner.cpp
#   - XCP：src/xcp/xcpdataparser.y + src/xcp/xcpdataflexer.l
# 关键事实（勿凭记忆改动）：
#   1) ASAP2 块一律 `/begin KEY ... /end KEY`（闭合关键字重复）；
#   2) `/begin IF_DATA XCP` 由主干词法器 ReadIfData() 整段吞入直到
#      `/endIF_DATA`（跳空白匹配），因此 XCP 段可以多行书写；
#   3) XCP 子词法器把 0x… 直接返回 UINT token，DAQ/ODT_ENTRY 的十六进制合法；
#   4) PROTOCOL_LAYER 的 ADDRESS_GRANULARITY ident 仅占位（上游不调用
#      SetAddressGranularity），GetAddressGranularity() 恒为默认 BYTE=1；
#   5) MEASUREMENT 必填 8 个定位参数（name desc datatype compu res acc lo hi）；
#   6) DAQ_LIST 必须嵌套在 DAQ 块内（daq_optional → daq_list）；
#   7) UDP 主机关键字是 HOST_NAME + STRING（不是 HOST / 不是 ADDRESS）。
#
# 用法：python gen_a2l.py --out <目录>
#   输出：<目录>/golden_basic.a2l + <目录>/expected.json
#   （expected.json 供人核对；A2lSmoke 用例内以常量形式固化同一组期望值）
#
# 期望值推导（独立于 a2llib）：
#   M_LINEAR：COEFFS_LINEAR 0.0 0.5 → SDK 记 o=0.0(偏移) c=0.5(比例)，
#             p = f + i*c + i*o = i/2；raw 0x00C8(=200) → 100.0。
#             0.5 为二进制精确值，正逆算均无浮点误差。
#   M_BYTE  ：IDENTICAL → raw 42 → 整数物理值 42。
#   DTO     ：PID(=EPK=1) + [C8 00] + [2A]。
# =============================================================================

import argparse
import json
from pathlib import Path

# ---------------------------------------------------------------------------
# 黄金样本规格（唯一事实来源；A2lSmoke 内常量与本表逐项对应）
# ---------------------------------------------------------------------------
SPEC = {
    "project": "SmokeProj",
    "module": "SMOKE_ECU",
    # 符号地址三件套（B-1：ECU_ADDRESS 原值 + 独立 8-bit extension）
    "symbols": {
        "M_ARRAY": {"datatype": "UWORD", "compu": "CM_IDENT",
                    "address": 0x8000, "ext": 0x12, "array_size": 4},
        "M_LINEAR": {"datatype": "UWORD", "compu": "CM_LINEAR",
                     "address": 0x8020, "ext": 0x12},
        "M_BYTE": {"datatype": "UBYTE", "compu": "CM_IDENT",
                   "address": 0x8010, "ext": 0x00},
    },
    # DAQ_LIST：EPK=1（PID 匹配用），PREDEFINED 单 ODT 两条 entry
    "daq_list": {
        "number": 1,
        "odt_number": 1,
        "entries": [
            {"number": 1, "address": 0x8020, "ext": 0x12, "size": 2, "bit_offset": 0},
            {"number": 2, "address": 0x8010, "ext": 0x00, "size": 1, "bit_offset": 0},
        ],
    },
    "protocol_layer": {"version": 0x0100, "max_cto": 0x10, "max_dto": 0x20},
    "udp": {"port": 0x15B7, "host": "localhost"},
    "linear_coeffs": {"offset": 0.0, "factor": 0.5},
}


def fmt_int(v: int) -> str:
    """按 A2L 习惯输出十六进制整数字面量。"""
    return f"0x{v:X}"


def emit_measurements(spec):
    """生成 MEASUREMENT 块（必填 8 定位参数 + 显式 BYTE_ORDER 属性）。"""
    lines = []
    for name in ("M_ARRAY", "M_BYTE", "M_LINEAR"):  # 字母序输出，便于 diff
        s = spec["symbols"][name]
        lines.append(
            f'    /begin MEASUREMENT {name} "smoke {name}" '
            f'{s["datatype"]} {s["compu"]} 0 0 0 65535'
        )
        lines.append('      BYTE_ORDER MSB_LAST')
        lines.append(f'      ECU_ADDRESS {fmt_int(s["address"])}')
        lines.append(f'      ECU_ADDRESS_EXTENSION {fmt_int(s["ext"])}')
        if "array_size" in s:
            lines.append(f'      ARRAY_SIZE {s["array_size"]}')
        if name != "M_ARRAY":  # 数组整体只读；标量给 FromPhysical 用例放行
            lines.append('      READ_WRITE')
        if name == "M_LINEAR":
            lines.append('      PHYS_UNIT "V"')
        lines.append('    /end MEASUREMENT')
    return lines


def emit_if_data_xcp(spec):
    """生成 MODULE 级 IF_DATA XCP 段（多行合法，见文件头事实 2）。"""
    pl = spec["protocol_layer"]
    dl = spec["daq_list"]
    udp = spec["udp"]
    lines = []
    lines.append('    /begin IF_DATA XCP')
    # PROTOCOL_LAYER：10 个 UINT（version + T1..T7 + MAX_CTO + MAX_DTO）
    # + BYTE_ORDER ident + ADDRESS_GRANULARITY ident（占位，上游不消费）。
    # 单行收口：`/begin` 与 `/end` 在同一行也不影响 ReadIfData 的整段吞入。
    lines.append(
        f'      /begin PROTOCOL_LAYER {fmt_int(pl["version"])} '
        '1 1 5 5 5 1 1 '
        f'{fmt_int(pl["max_cto"])} {fmt_int(pl["max_dto"])} '
        'BYTE_ORDER_MSB_LAST ADDRESS_GRANULARITY_BYTE /end PROTOCOL_LAYER')
    # DAQ：token 顺序 = type max_daq max_event min_daq optimisation addr_ext
    #       id_field granularity max_odt_entry_size overload（xcpdataparser.y:271）
    lines.append(
        '      /begin DAQ STATIC 3 2 0 '
        'OPTIMISATION_TYPE_DEFAULT ADDRESS_EXTENSION_DAQ '
        'IDENTIFICATION_FIELD_TYPE_ABSOLUTE '
        'GRANULARITY_ODT_ENTRY_SIZE_DAQ_BYTE 4 OVERLOAD_INDICATION_EVENT')
    # DAQ_LIST 嵌套于 DAQ 的 optional 项中（daq_optional → daq_list）
    lines.append(f'        /begin DAQ_LIST {dl["number"]}')
    lines.append('          DAQ_LIST_TYPE DAQ')
    lines.append('          MAX_ODT 1')
    lines.append('          MAX_ODT_ENTRIES 2')
    lines.append('          FIRST_PID 1')
    lines.append(f'          EVENT_FIXED {dl["number"]}')
    lines.append('          /begin PREDEFINED')
    lines.append(f'            /begin ODT {dl["odt_number"]}')
    for e in dl["entries"]:
        lines.append(
            f'              ODT_ENTRY {e["number"]} {fmt_int(e["address"])} '
            f'{fmt_int(e["ext"])} {e["size"]} {e["bit_offset"]}')
    lines.append('            /end ODT')
    lines.append('          /end PREDEFINED')
    lines.append('        /end DAQ_LIST')
    lines.append('      /end DAQ')
    # UDP/IP 传输层（SDK 读 GetPort()/GetHostName()）
    lines.append(
        f'      /begin XCP_ON_UDP_IP {fmt_int(pl["version"])} {fmt_int(udp["port"])}')
    lines.append(f'        HOST_NAME "{udp["host"]}"')
    lines.append('      /end XCP_ON_UDP_IP')
    lines.append('    /end IF_DATA')
    return lines


def build_a2l(spec):
    """组装完整 A2L 文本（行列表）。"""
    lines = []
    lines.append('ASAP2_VERSION 1 61')
    lines.append('')
    lines.append(f'/begin PROJECT {spec["project"]} "libxcp A2L minimal smoke sample"')
    lines.append('')
    lines.append(f'  /begin MODULE {spec["module"]} "single module for smoke"')
    lines.append('')
    lines.append('    /begin MOD_COMMON "smoke"')
    lines.append('      BYTE_ORDER MSB_LAST')
    lines.append('      DEPOSIT ABSOLUTE')
    lines.append('      ALIGNMENT_BYTE 1')
    lines.append('      ALIGNMENT_WORD 1')
    lines.append('      ALIGNMENT_LONG 1')
    lines.append('    /end MOD_COMMON')
    lines.append('')
    lines.append('    /begin MOD_PAR "smoke par"')
    lines.append('      CPU_TYPE "SMOKE-CPU"')
    lines.append('    /end MOD_PAR')
    lines.append('')
    # COMPU_METHOD：IDENTICAL（恒等）与 LINEAR（COEFFS_LINEAR offset factor）
    lines.append('    /begin COMPU_METHOD CM_IDENT "identity" IDENTICAL "%9" ""')
    lines.append('    /end COMPU_METHOD')
    lin = spec["linear_coeffs"]
    lines.append('    /begin COMPU_METHOD CM_LINEAR "p = i/2" LINEAR "%9" "V"')
    lines.append(f'      COEFFS_LINEAR {lin["offset"]} {lin["factor"]}')
    lines.append('    /end COMPU_METHOD')
    lines.append('')
    lines.extend(emit_measurements(spec))
    lines.append('')
    # CHARACTERISTIC（首里程碑仅元数据：SDK 不推导 RECORD_LAYOUT 元素类型）
    lines.append(
        f'    /begin CHARACTERISTIC C_VALUE "smoke C_VALUE" VALUE '
        f'0x9000 STD_VALUE 0.0 CM_LINEAR 0 10000')
    lines.append('      BYTE_ORDER MSB_LAST')
    lines.append('      ECU_ADDRESS_EXTENSION 0x00')
    lines.append('    /end CHARACTERISTIC')
    lines.append('')
    lines.extend(emit_if_data_xcp(spec))
    lines.append('')
    lines.append('  /end MODULE')
    lines.append('')
    lines.append('/end PROJECT')
    return lines


def build_expected(spec):
    """由 spec 独立反算期望值（raw↔phys、地址推进、DTO 帧），不经 a2llib。"""
    lin = spec["linear_coeffs"]
    symbols = {}
    for name, s in spec["symbols"].items():
        item = {
            "module": spec["module"],
            "qualified": f'{spec["module"]}::{name}',
            "datatype": s["datatype"],
            "element_size_bytes": {"UBYTE": 1, "UWORD": 2}[s["datatype"]],
            "xcp_address": s["address"],
            "address_extension": s["ext"],
            "read_write": name != "M_ARRAY",
        }
        if "array_size" in s:
            item["dimensions"] = [
                {"extent": s["array_size"], "byte_stride": item["element_size_bytes"]}]
            item["byte_size"] = s["array_size"] * item["element_size_bytes"]
            # B-1：idx2（AG=Byte）→ base+4；AG=Word → base+2（元素宽2/AG2=1步）
            item["element_address_idx2_byte"] = \
                s["address"] + 2 * item["element_size_bytes"]
            item["element_address_idx2_word"] = \
                s["address"] + (2 * item["element_size_bytes"]) // 2
        symbols[name] = item
    # LINEAR：raw 200 → phys = f + i*c + i*o = 0 + 200*0.5 + 200*0 = 100.0
    raw_linear = 200
    phys_linear = lin["offset"] + raw_linear * lin["factor"] + raw_linear * 0.0
    symbols["M_LINEAR"]["sample_raw"] = [raw_linear & 0xFF, raw_linear >> 8]
    symbols["M_LINEAR"]["sample_phys"] = phys_linear
    # IDENTICAL：raw 42 → 整数 42
    symbols["M_BYTE"]["sample_raw"] = [0x2A]
    symbols["M_BYTE"]["sample_phys"] = 42

    dl = spec["daq_list"]
    dto_frame = [dl["number"]]  # PID == EPK（绝对标识模式）
    raws = {0: symbols["M_LINEAR"]["sample_raw"],
            1: symbols["M_BYTE"]["sample_raw"]}
    for i, e in enumerate(dl["entries"]):
        dto_frame.extend(raws[i])
    return {
        "symbols": symbols,
        "protocol_layer": spec["protocol_layer"],
        "udp": spec["udp"],
        "daq_list": {"number": dl["number"],
                     "entries": [{"address": e["address"], "ext": e["ext"],
                                  "size": e["size"]} for e in dl["entries"]]},
        "dto_frame": dto_frame,
    }


def main():
    parser = argparse.ArgumentParser(description="生成最小 A2L 黄金样本")
    parser.add_argument("--out", required=True, help="输出目录")
    args = parser.parse_args()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    a2l_path = out_dir / "golden_basic.a2l"
    # CRLF 行尾：贴近真实工具导出，同时验证主干词法器对 \r 的处理
    a2l_path.write_text("\r\n".join(build_a2l(SPEC)) + "\r\n", encoding="ascii")
    exp_path = out_dir / "expected.json"
    exp_path.write_text(json.dumps(build_expected(SPEC), ensure_ascii=False, indent=2),
                        encoding="utf-8")
    print(f"generated: {a2l_path}")
    print(f"expected : {exp_path}")


if __name__ == "__main__":
    main()
