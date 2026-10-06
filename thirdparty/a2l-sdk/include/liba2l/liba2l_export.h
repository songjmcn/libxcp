// ============================================================================
// liba2l 导出宏（设计文档 §5.3.3b）
//
// 规则：
//   - 构建 liba2l.dll 时，SDK CMake 对目标定义 LIBA2L_MAKE_SHARED → __declspec(dllexport)
//   - 消费方（libxcp_a2lbridge / A2lSmoke）什么都不定义 → 默认分支 __declspec(dllimport)
//   - 定义 LIBA2L_STATIC_LINK 时宏为空（未来静态交付形态预留，当前不使用）
//
// 用法：标注在导出的抽象类与工厂函数上（class A2L_INTERFACE IDoc {...}），
// 整类导出可覆盖全部成员（含内联），避免使用 WINDOWS_EXPORT_ALL_SYMBOLS。
// ============================================================================

#ifndef LIBA2L_LIBA2L_EXPORT_H_
#define LIBA2L_LIBA2L_EXPORT_H_

// ----------------------------------------------------------------------------
// 字符串化辅助（用于 ABI 名字修饰检查等）
// ----------------------------------------------------------------------------
#define LIBA2L_STRINGIFY_IMPL(x) #x
#define LIBA2L_STRINGIFY(x) LIBA2L_STRINGIFY_IMPL(x)

#if defined(LIBA2L_STATIC_LINK)

// 静态链接形态：不区分导入/导出
#define A2L_INTERFACE
#define A2L_LOCAL

#elif defined(_WIN32) || defined(__CYGWIN__)

// Windows：__declspec 显式导出/导入
#ifdef LIBA2L_MAKE_SHARED
#define A2L_INTERFACE __declspec(dllexport)
#else
#define A2L_INTERFACE __declspec(dllimport)
#endif
#define A2L_LOCAL

#else

// GCC / Clang：默认可见性 + hidden 局部化内部符号
#if defined(LIBA2L_MAKE_SHARED) && defined(__GNUC__) && (__GNUC__ >= 4)
#define A2L_INTERFACE __attribute__((visibility("default")))
#else
#define A2L_INTERFACE
#endif
#define A2L_LOCAL __attribute__((visibility("hidden")))

#endif

#endif  // LIBA2L_LIBA2L_EXPORT_H_
