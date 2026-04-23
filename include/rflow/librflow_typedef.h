/**
 * @file     librflow_typedef.h
 * @brief    类型定义
 *
 * 官方构建支持：GCC / Clang（Linux arm64、Android arm64）。
 * 该头文件允许在任意 C/C++ 编译器下被消费端 include（仅声明，不实际链接）；
 * 非 GCC/Clang 下 LIBRFLOW_API_EXPORT 与 LIBRFLOW_API_DEPRECATED_MSG 退化为空，
 * 以便跨平台项目能共享同一份头文件进行静态检查或生成 FFI binding。
**/

#ifndef __LIBRFLOW_TYPEDEF_H__
#define __LIBRFLOW_TYPEDEF_H__

#include <stdbool.h>
#include <stdint.h>

#ifndef LIBRFLOW_API_DEPRECATED_MSG
#if defined(__GNUC__) || defined(__clang__)
#define LIBRFLOW_API_DEPRECATED_MSG(func) \
    __attribute__((__deprecated__("Use " func " instead")))
#elif defined(_MSC_VER)
#define LIBRFLOW_API_DEPRECATED_MSG(func) __declspec(deprecated("Use " func " instead"))
#else
#define LIBRFLOW_API_DEPRECATED_MSG(func)
#endif
#endif

#ifdef __cplusplus
#ifndef LIBRFLOW_API_EXPORT
#define LIBRFLOW_API_EXPORT extern "C"
#endif
#ifndef LIBRFLOW_DEPRECATED_EXPORT
#define LIBRFLOW_DEPRECATED_EXPORT(func) extern "C" LIBRFLOW_API_DEPRECATED_MSG(func)
#endif
#else
#ifndef LIBRFLOW_API_EXPORT
#define LIBRFLOW_API_EXPORT
#endif
#ifndef LIBRFLOW_DEPRECATED_EXPORT
#define LIBRFLOW_DEPRECATED_EXPORT(func) LIBRFLOW_API_DEPRECATED_MSG(func)
#endif
#endif

/*
 * 对外 API 统一使用 <stdint.h> / <stdbool.h> 的标准类型
 * (int32_t / uint32_t / bool / ...)，不再暴露 LIBRFLOW_* 别名。
 */

#endif /* __LIBRFLOW_TYPEDEF_H__ */
