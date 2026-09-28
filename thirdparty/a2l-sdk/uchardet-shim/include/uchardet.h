/*
 * uchardet 最小 API 桩声明
 *
 * 仅声明上游 a2llib 实际引用的符号（实测分布：src/a2lhelper.cpp 内
 * DetectCharset() 使用 new/handle_data/data_end/get_charset/delete）。
 * 为兼容未来上游代码变化，附带真 uchardet 公开的另一组常用函数声明。
 *
 * 与真 uchardet.h 保持同名、同 C 链接，使上游 #include <uchardet.h> 无感切换。
 */

#ifndef UCHARDET_H_
#define UCHARDET_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** uchardet 探测句柄（不透明指针，桩实现里只是堆上的哨兵对象）。 */
typedef void* uchardet_t;

/**
 * 创建探测器句柄。
 * @return 成功返回非空句柄；桩实现总是返回一个可用句柄。
 */
uchardet_t uchardet_new(void);

/**
 * 销毁探测器句柄。
 * @param ud [in] uchardet_new() 返回的句柄，可为 NULL（桩实现容忍）。
 */
void uchardet_delete(uchardet_t ud);

/**
 * 喂入待探测数据。
 * @param ud   [in] 句柄。
 * @param data [in] 数据缓冲区。
 * @param len  [in] 数据长度（字节）。
 * @return 0 表示成功；非 0 表示失败（桩实现总是返回 0）。
 */
int uchardet_handle_data(uchardet_t ud, const char* data, size_t len);

/** 通知数据输入结束（桩实现为空操作）。 */
void uchardet_data_end(uchardet_t ud);

/**
 * 获取探测出的字符集名称。
 * @param ud [in] 句柄。
 * @return 字符集名字符串；桩实现固定返回空串 ""，触发上游"未识别编码"回退路径。
 */
const char* uchardet_get_charset(uchardet_t ud);

/**
 * 获取候选编码数量。
 * @param ud [in] 句柄。
 * @return 候选个数；桩实现固定返回 0。
 */
int uchardet_get_n_candidates(uchardet_t ud);

/**
 * 按序号获取候选编码名。
 * @param ud     [in] 句柄。
 * @param index  [in] 候选序号。
 * @return 编码名；桩实现固定返回空串 ""。
 */
const char* uchardet_get_candidate(uchardet_t ud, int index);

/**
 * 获取探测出的具体编码（区别于 get_charset 的宽松名）。
 * @param ud [in] 句柄。
 * @return 编码名；桩实现固定返回空串 ""。
 */
const char* uchardet_get_encoding(uchardet_t ud);

#ifdef __cplusplus
}
#endif

#endif  // UCHARDET_H_
