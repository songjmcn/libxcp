/**
 * @file dto_payload_decoder.cpp
 * @brief 实现 ODT 净荷字节级切片解码（v0.5，核心，A2L-free）。
 *
 * 见 dto_payload_decoder.hpp 头文件语义：纯字节切片，越界切片 valid=false，
 * 不抛异常，物理换算由上层经 IMeasurementDatabase 完成。
 */

#include "libxcp/daq/dto_payload_decoder.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace calmcar::xcp {

std::vector<DecodedSlice> DecodePayload(
    BytesView payload, const std::vector<PayloadSlice>& specs) noexcept {
    std::vector<DecodedSlice> out;
    out.reserve(specs.size());
    for (const PayloadSlice& spec : specs) {
        DecodedSlice slice;
        slice.name = spec.name;
        slice.valid = (spec.size == 0) ||
                      (spec.offset <= payload.size() &&
                       spec.size <= payload.size() - spec.offset);
        if (slice.valid && spec.size > 0) {
            slice.raw.assign(payload.begin() +
                                 static_cast<std::ptrdiff_t>(spec.offset),
                             payload.begin() +
                                 static_cast<std::ptrdiff_t>(spec.offset +
                                                             spec.size));
        }
        out.push_back(std::move(slice));
    }
    return out;
}

}  // namespace calmcar::xcp