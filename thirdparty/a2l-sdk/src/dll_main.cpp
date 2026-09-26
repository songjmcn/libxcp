// ============================================================================
// liba2l.dll 入口（设计文档 §5.3）
//
// 本 DLL 无需任何自定义初始化/清理逻辑：全部状态挂在 IDoc 对象上，
// CRT 启动例程足够。此文件存在的意义仅是让工程结构显式表达"这是一个
// Windows DLL 目标"，并作为将来接入 DllMain 钩子（如进程 attach 校验）的落点。
// ============================================================================

#include <windows.h>

/**
 * @brief DLL 入口点。
 * @param hinstDLL 模块实例句柄。
 * @param fdwReason 触发原因。
 * @param lpvReserved 保留参数。
 * @return TRUE 允许加载；PROCESS_DETACH 下不做任何清理直接返回 TRUE。
 */
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    UNREFERENCED_PARAMETER(hinstDLL);
    UNREFERENCED_PARAMETER(lpvReserved);
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
        case DLL_PROCESS_DETACH:
            break;
        default:
            return FALSE;
    }
    return TRUE;
}
