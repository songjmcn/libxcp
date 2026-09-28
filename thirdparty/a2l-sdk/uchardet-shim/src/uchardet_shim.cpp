/*
 * uchardet 桩实现（设计文档 §5.2 方案 A / A-8 裁决）
 *
 * 语义：所有探测一律返回"未知编码"。上游 a2lhelper.cpp::DetectCharset() 拿到
 * 空 charset 后，A2lHelper::GetCharset() 返回空串，a2lscanner 不登记 encoding，
 * ConvertAllStrings() 因 encoding 为空直接早退 —— 即非 UTF-8 文件不再做字符集
 * 猜测，按原始字节透传。首里程碑测试样本统一 UTF-8/ASCII，行为完全正确。
 *
 * 若未来需要真编码探测，替换本桩为真 uchardet（或 iconv 桥），接口不变。
 */

#include <cstdlib>
#include <cstring>

#include "uchardet.h"

namespace {

/** 哨兵对象：句柄仅用于区分 NULL 与非 NULL，无内部状态需求。 */
struct ShimDetector {
    /** 占位成员，保证每个实例有独立地址。 */
    char marker_;
};

/** 桩固定返回的空字符串（生命周期与进程一致）。 */
constexpr char kEmpty[] = "";

}  // namespace

extern "C" {

uchardet_t uchardet_new(void) {
    auto* det = static_cast<ShimDetector*>(std::malloc(sizeof(ShimDetector)));
    if (det != nullptr) {
        det->marker_ = 'u';
    }
    return static_cast<uchardet_t>(det);
}

void uchardet_delete(uchardet_t ud) { std::free(ud); }

int uchardet_handle_data(uchardet_t ud, const char* data, size_t len) {
    // 桩不做任何探测；参数校验保持与真库一致的宽容度（NULL 句柄视为失败）。
    (void)data;
    (void)len;
    return ud == nullptr ? -1 : 0;
}

void uchardet_data_end(uchardet_t ud) { (void)ud; }

const char* uchardet_get_charset(uchardet_t ud) {
    (void)ud;
    return kEmpty;
}

int uchardet_get_n_candidates(uchardet_t ud) {
    (void)ud;
    return 0;
}

const char* uchardet_get_candidate(uchardet_t ud, int index) {
    (void)ud;
    (void)index;
    return kEmpty;
}

const char* uchardet_get_encoding(uchardet_t ud) {
    (void)ud;
    return kEmpty;
}

}  // extern "C"
