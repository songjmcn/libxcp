// tests/measurement_planner_test.cpp
//
// 测量规划器（MeasurementPlanner）L1 单元测试。
//
// 覆盖计划 §5.3 / §5.5 硬核对项：
//   - 按 event_channel 分组（event 0 独立成组）且组内保持用户顺序；
//   - 恰好在 usable 上限内装箱不溢出，超出则开新 ODT；
//   - 单变量超过 usable（MAX_DTO 扣掉信封头）→ DaqConfigurationError；
//   - AG 单位换算（字节 → DaqEntrySpec.size）+ 不可整除 → InvalidLayout；
//   - timestamp 头扣除（usable 变小 + DaqListSpec.timestamp 置位）；
//   - 未知符号名/8 位 AG 上限 → 报错；
//   - 多 event 多 List：daq_list 严格 0..N-1；
//   - 空输入 → 空计划。
//
// 使用一个内存 FakeDb 实现 IMeasurementDatabase，不依赖 A2L。

#include <libxcp/measurement/measurement_database.hpp>
#include <libxcp/measurement/measurement_planner.hpp>
#include <libxcp/measurement/measurement_result.hpp>
#include <libxcp/xcp_master.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace calmcar::xcp {
namespace {

// 内存版测量数据库：symbols 表 name → MeasurementSymbolInfo。
class FakeDb final : public IMeasurementDatabase {
 public:
  FakeDb() = default;  // 基类删除拷贝构造，显式恢复默认构造
  MeasurementResult<MeasurementSymbolInfo> Find(
      std::string_view name) const override {
    const auto it = symbols.find(std::string(name));
    if (it == symbols.end()) {
      return detail::MakeMeasurementError<MeasurementSymbolInfo>(
          MeasurementErrorCode::NotFound, "未找到测量: " + std::string(name),
          std::string(name));
    }
    return detail::MakeMeasurementOk(it->second);
  }

  // IMeasurementDatabase override（规划阶段不会被调用）
  MeasurementResult<MeasurementValue> ToPhysical(
      std::string_view /*name*/, BytesView /*raw*/) const override {
    return detail::MakeMeasurementOk<MeasurementValue>(
        MeasurementValue{std::int64_t{0}});
  }

  std::map<std::string, MeasurementSymbolInfo> symbols;
};

// 构造一个符号信息。
MeasurementSymbolInfo MakeInfo(uint32_t address, uint8_t extension,
                               uint16_t event_channel,
                               uint8_t element_size_bytes,
                               uint8_t element_count = 1) {
  return MeasurementSymbolInfo{address, extension, event_channel,
                               element_size_bytes, element_count};
}

// 校验给定计划：每条 route 都能回查到所属 (daq_list, odt, payload_offset)，
// 且同一 ODT 内净荷字节总和 ≤ usable。
void AssertRoutesConsistent(const MeasurementPlan& plan, size_t usable_bytes,
                            size_t timestamp_bytes, size_t ag_bytes) {
  const std::size_t id_bytes = 1;  // 识别字段固定 1 字节（规划器假定）
  const std::size_t usable = usable_bytes - timestamp_bytes - id_bytes;
  for (const auto& route : plan.routes) {
    ASSERT_LT(route.daq_list, plan.daq_lists.size());
    const auto& list = plan.daq_lists[route.daq_list];
    ASSERT_LT(route.odt, list.odts.size());
    const auto& odt = list.odts[route.odt];
    // 净荷字节 = Σ entry.size * ag_bytes。
    std::size_t sum = 0;
    for (const auto& e : odt.entries) {
      sum += static_cast<std::size_t>(e.size) * ag_bytes;
    }
    ASSERT_LE(sum, usable) << "ODT " << route.odt << " of list "
                           << route.daq_list << " exceeds usable " << usable;
    ASSERT_LE(route.payload_offset + route.size, sum)
        << "route '" << route.name << "' overruns its ODT";
  }
}

// ---------------------------------------------------------------------------
// 分组：多 event + event 0 独立成组
// ---------------------------------------------------------------------------
TEST(MeasurementPlannerTest, GroupsByEventChannelWithEventZeroOwnGroup) {
  FakeDb db;
  db.symbols.emplace("ev0_a", MakeInfo(0x1000, 0, 0, 4));   // event 0
  db.symbols.emplace("ev1_a", MakeInfo(0x2000, 0, 1, 4));   // event 1
  db.symbols.emplace("ev0_b", MakeInfo(0x1004, 0, 0, 4));   // event 0
  db.symbols.emplace("ev1_b", MakeInfo(0x2004, 0, 1, 4));   // event 1
  db.symbols.emplace("ev2_a", MakeInfo(0x3000, 0, 2, 4));   // event 2

  MeasurementPlanner planner(db, /*max_dto=*/128, AddressGranularity::Byte);

  const auto result = planner.Build({"ev0_a", "ev1_a", "ev0_b", "ev1_b", "ev2_a"});
  ASSERT_TRUE(result.HasValue()) << result.ErrorInfo().message;
  const auto& plan = result.Value();

  // 3 个 event → 3 个 List
  ASSERT_EQ(plan.daq_lists.size(), 3U);
  // event 0 组排在前面（稳定第一位）
  EXPECT_EQ(plan.daq_lists[0].event_channel, 0U);
  EXPECT_EQ(plan.daq_lists[1].event_channel, 1U);
  EXPECT_EQ(plan.daq_lists[2].event_channel, 2U);
  // daq_list 严格 0..2
  EXPECT_EQ(plan.daq_lists[0].daq_list, 0U);
  EXPECT_EQ(plan.daq_lists[1].daq_list, 1U);
  EXPECT_EQ(plan.daq_lists[2].daq_list, 2U);

  // route 顺序 = event0 全组 → event1 全组 → event2
  ASSERT_EQ(plan.routes.size(), 5U);
  EXPECT_EQ(plan.routes[0].name, "ev0_a");
  EXPECT_EQ(plan.routes[1].name, "ev0_b");
  EXPECT_EQ(plan.routes[2].name, "ev1_a");
  EXPECT_EQ(plan.routes[3].name, "ev1_b");
  EXPECT_EQ(plan.routes[4].name, "ev2_a");

  AssertRoutesConsistent(plan, 128, 0, /*ag_bytes=*/1);
}

// ---------------------------------------------------------------------------
// 组内保持用户顺序
// ---------------------------------------------------------------------------
TEST(MeasurementPlannerTest, PreservesUserOrderWithinGroup) {
  FakeDb db;
  db.symbols.emplace("b", MakeInfo(0x1100, 0, 1, 4));
  db.symbols.emplace("a", MakeInfo(0x1104, 0, 1, 4));
  db.symbols.emplace("c", MakeInfo(0x1108, 0, 1, 4));

  MeasurementPlanner planner(db, /*max_dto=*/128, AddressGranularity::Byte);
  const auto result = planner.Build({"b", "a", "c"});
  ASSERT_TRUE(result.HasValue()) << result.ErrorInfo().message;
  ASSERT_EQ(result.Value().routes.size(), 3U);
  EXPECT_EQ(result.Value().routes[0].name, "b");
  EXPECT_EQ(result.Value().routes[1].name, "a");
  EXPECT_EQ(result.Value().routes[2].name, "c");
}

// ---------------------------------------------------------------------------
// 恰好填满 usable 不开新 ODT；超出才开新 ODT（同 daq_list，odt++）
// ---------------------------------------------------------------------------
TEST(MeasurementPlannerTest, FullOdtThenOverflowOpensNewOdt) {
  FakeDb db;
  // max_dto=24，timestamp_bytes=0 → usable = 24 - 0 - 1 = 23
  // 变量实占 11 字节：第一个装满(11+11=22 ≤23)，第三个需要新 ODT(22+11=33>23)
  db.symbols.emplace("x", MakeInfo(0x1000, 0, 5, 11));
  db.symbols.emplace("y", MakeInfo(0x100B, 0, 5, 11));
  db.symbols.emplace("z", MakeInfo(0x1016, 0, 5, 11));

  MeasurementPlanner planner(db, /*max_dto=*/24, AddressGranularity::Byte);
  const auto result = planner.Build({"x", "y", "z"});
  ASSERT_TRUE(result.HasValue()) << result.ErrorInfo().message;
  const auto& plan = result.Value();

  // 单一 event → 单一 List
  ASSERT_EQ(plan.daq_lists.size(), 1U);
  // List 内 2 个 ODT
  ASSERT_EQ(plan.daq_lists[0].odts.size(), 2U);
  // 前两个 route 落 ODT0，第三个落 ODT1
  ASSERT_EQ(plan.routes.size(), 3U);
  EXPECT_EQ(plan.routes[0].odt, 0U);
  EXPECT_EQ(plan.routes[1].odt, 0U);
  EXPECT_EQ(plan.routes[2].odt, 1U);
  // route[2] 在新 ODT 的偏移应回到 0
  EXPECT_EQ(plan.routes[2].payload_offset, 0U);
  EXPECT_EQ(plan.routes[0].payload_offset, 0U);
  EXPECT_EQ(plan.routes[1].payload_offset, 11U);

  AssertRoutesConsistent(plan, 24, 0, /*ag_bytes=*/1);
}

// ---------------------------------------------------------------------------
// 单变量超过 usable → DaqConfigurationError
// ---------------------------------------------------------------------------
TEST(MeasurementPlannerTest, SingleEntryLargerThanUsableErrors) {
  FakeDb db;
  // max_dto=16 → usable = 16 - 0 - 1 = 15；变量 16 字节放不下
  db.symbols.emplace("big", MakeInfo(0x1000, 0, 1, 16));

  MeasurementPlanner planner(db, /*max_dto=*/16, AddressGranularity::Byte);
  const auto result = planner.Build({"big"});
  ASSERT_FALSE(result.HasValue());
  EXPECT_EQ(result.ErrorInfo().code, MeasurementErrorCode::DaqConfigurationError);
}

// ---------------------------------------------------------------------------
// AG 换算：Word 粒度下 entry.size = byte_width / 2
// ---------------------------------------------------------------------------
TEST(MeasurementPlannerTest, AgConversionWordGranularity) {
  FakeDb db;
  // 实占 4 字节，AG=Word(2) → size = 2
  db.symbols.emplace("w", MakeInfo(0x1000, 0, 3, 4));

  MeasurementPlanner planner(db, /*max_dto=*/64, AddressGranularity::Word);
  const auto result = planner.Build({"w"});
  ASSERT_TRUE(result.HasValue()) << result.ErrorInfo().message;
  const auto& list = result.Value().daq_lists[0];
  ASSERT_EQ(list.odts.size(), 1U);
  ASSERT_EQ(list.odts[0].entries.size(), 1U);
  // DaqEntrySpec.size 为 AG 单位
  EXPECT_EQ(list.odts[0].entries[0].size, 2U);
  EXPECT_EQ(result.Value().routes[0].size, 4U);  // route 记录实占字节
}

// ---------------------------------------------------------------------------
// 不可被 AG 整除 → InvalidLayout
// ---------------------------------------------------------------------------
TEST(MeasurementPlannerTest, ByteWidthNotDivisibleByAgErrors) {
  FakeDb db;
  // 实占 3 字节，AG=Word(2)：3 % 2 != 0 → InvalidLayout
  db.symbols.emplace("v", MakeInfo(0x1000, 0, 1, 3));

  MeasurementPlanner planner(db, /*max_dto=*/64, AddressGranularity::Word);
  const auto result = planner.Build({"v"});
  ASSERT_FALSE(result.HasValue());
  EXPECT_EQ(result.ErrorInfo().code, MeasurementErrorCode::InvalidLayout);
  // 符号也写上符号名
  EXPECT_EQ(result.ErrorInfo().symbol, "v");
}

// ---------------------------------------------------------------------------
// timestamp 头扣除：usable 变小 + timestamp 置位
// ---------------------------------------------------------------------------
TEST(MeasurementPlannerTest, TimestampHeaderDeduction) {
  FakeDb db;
  // max_dto=16, timestamp_bytes=4 → usable = 16 - 4 - 1 = 11
  // 变量 11 字节 → 恰好；12 字节 → 超出
  db.symbols.emplace("fits", MakeInfo(0x1000, 0, 1, 11));
  db.symbols.emplace("too_big", MakeInfo(0x1100, 0, 1, 12));

  // fits 能放下
  {
    MeasurementPlanner planner(db, /*max_dto=*/16, AddressGranularity::Byte,
                                /*timestamp_bytes=*/4);
    const auto ok = planner.Build({"fits"});
    ASSERT_TRUE(ok.HasValue()) << ok.ErrorInfo().message;
    // timestamp 头应使 DaqListSpec.timestamp 置位
    EXPECT_TRUE(ok.Value().daq_lists[0].timestamp);
    // header_bytes 也由上层最终按运行时取证补齐，这里仅验证净荷
    AssertRoutesConsistent(ok.Value(), 16, 4, /*ag_bytes=*/1);
  }
  // too_big（12 > 11）放不下
  {
    MeasurementPlanner planner(db, /*max_dto=*/16, AddressGranularity::Byte,
                                       /*timestamp_bytes=*/4);
    const auto bad = planner.Build({"too_big"});
    ASSERT_FALSE(bad.HasValue());
    EXPECT_EQ(bad.ErrorInfo().code, MeasurementErrorCode::DaqConfigurationError);
  }
}

// ---------------------------------------------------------------------------
// 未知符号名 → error
// ---------------------------------------------------------------------------
TEST(MeasurementPlannerTest, UnknownNameErrors) {
  FakeDb db;  // 空表
  MeasurementPlanner planner(db, /*max_dto=*/128, AddressGranularity::Byte);
  const auto result = planner.Build({"missing"});
  ASSERT_FALSE(result.HasValue());
  EXPECT_EQ(result.ErrorInfo().code, MeasurementErrorCode::NotFound);
  EXPECT_EQ(result.ErrorInfo().symbol, "missing");
}

// ---------------------------------------------------------------------------
// 多 event 多 List：daq_list 严格 0..N-1，route 可回查
// ---------------------------------------------------------------------------
TEST(MeasurementPlannerTest, MultiEventMultiList) {
  FakeDb db;
  db.symbols.emplace("e7_a", MakeInfo(0x1000, 0, 7, 4));
  db.symbols.emplace("e3_a", MakeInfo(0x2000, 0, 3, 4));
  db.symbols.emplace("e7_b", MakeInfo(0x1004, 0, 7, 4));
  db.symbols.emplace("e3_b", MakeInfo(0x2004, 0, 3, 4));

  MeasurementPlanner planner(db, /*max_dto=*/64, AddressGranularity::Byte);
  const auto result = planner.Build({"e7_a", "e3_a", "e7_b", "e3_b"});
  ASSERT_TRUE(result.HasValue()) << result.ErrorInfo().message;
  const auto& plan = result.Value();
  // 2 个 event（0 不在其中）→ 2 个 List
  ASSERT_EQ(plan.daq_lists.size(), 2U);
  EXPECT_EQ(plan.daq_lists[0].event_channel, 7U);
  EXPECT_EQ(plan.daq_lists[0].daq_list, 0U);
  EXPECT_EQ(plan.daq_lists[1].event_channel, 3U);
  EXPECT_EQ(plan.daq_lists[1].daq_list, 1U);

  // 分组按首现顺序：group 7 在前、group 3 在后；每组内保持用户顺序。
  // 输入顺序 (e7_a, e3_a, e7_b, e3_b) → 输出 (e7_a, e7_b, e3_a, e3_b)
  ASSERT_EQ(plan.routes.size(), 4U);
  EXPECT_EQ(plan.routes[0].name, "e7_a");
  EXPECT_EQ(plan.routes[0].daq_list, 0U);
  EXPECT_EQ(plan.routes[1].name, "e7_b");
  EXPECT_EQ(plan.routes[1].daq_list, 0U);
  EXPECT_EQ(plan.routes[2].name, "e3_a");
  EXPECT_EQ(plan.routes[2].daq_list, 1U);
  EXPECT_EQ(plan.routes[3].name, "e3_b");
  EXPECT_EQ(plan.routes[3].daq_list, 1U);

  AssertRoutesConsistent(plan, 64, 0, /*ag_bytes=*/1);
}

// ---------------------------------------------------------------------------
// 8 位 AG 上限（size 是 uint8）→ DaqConfigurationError
// ---------------------------------------------------------------------------
TEST(MeasurementPlannerTest, AgUnitsOver255Errors) {
  FakeDb db;
  // AG=Byte → ag_units == byte_width。宽度 = 元素宽度 × 元素数 = 100 × 3 = 300，
  // 元素宽度/数量都不超 uint8（分别 100 / 3），但总宽 300 > 255 → 报错。
  db.symbols.emplace("huge", MakeInfo(0x1000, 0, 1, /*esize=*/100,
                                      /*ecount=*/3));

  MeasurementPlanner planner(db, /*max_dto=*/512, AddressGranularity::Byte);
  const auto result = planner.Build({"huge"});
  // 300 > usable(512-0-1=511) 不触发单变量报错，但 300 > 255 触发 AG 上限
  ASSERT_FALSE(result.HasValue());
  EXPECT_EQ(result.ErrorInfo().code, MeasurementErrorCode::DaqConfigurationError);
}

// ---------------------------------------------------------------------------
// 空输入 → 空计划（无 List、无 route）
// ---------------------------------------------------------------------------
TEST(MeasurementPlannerTest, EmptyNamesYieldsEmptyPlan) {
  FakeDb db;
  MeasurementPlanner planner(db, /*max_dto=*/128, AddressGranularity::Byte);
  const auto result = planner.Build({});
  ASSERT_TRUE(result.HasValue()) << result.ErrorInfo().message;
  EXPECT_TRUE(result.Value().daq_lists.empty());
  EXPECT_TRUE(result.Value().routes.empty());
}

// ---------------------------------------------------------------------------
// 默认 spec 字段：prescaler=1, priority=0, 无 DTO 计数 / PID OFF / STIM
// ---------------------------------------------------------------------------
TEST(MeasurementPlannerTest, DefaultSpecFields) {
  FakeDb db;
  db.symbols.emplace("s", MakeInfo(0x1000, 0, 4, 4));

  MeasurementPlanner planner(db, /*max_dto=*/64, AddressGranularity::Byte);
  const auto result = planner.Build({"s"});
  ASSERT_TRUE(result.HasValue()) << result.ErrorInfo().message;
  const auto& spec = result.Value().daq_lists[0];
  EXPECT_EQ(spec.prescaler, 1);
  EXPECT_EQ(spec.priority, 0);
  EXPECT_FALSE(spec.dto_counter);
  EXPECT_FALSE(spec.stim_direction);
  EXPECT_FALSE(spec.pid_off);
  EXPECT_FALSE(spec.timestamp);  // timestamp_bytes=0 → 不置位
}

}  // namespace
}  // namespace calmcar::xcp