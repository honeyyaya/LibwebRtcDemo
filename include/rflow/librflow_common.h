/**
 * @file     librflow_common.h
 * @brief    Client / Service 两端共享的基础类型、枚举、共享 opaque 对象与 API
 *
 * Client 头（Client/librflow_client_api.h）与 Service 头（Service/librflow_service_api.h）
 * 均 include 本文件。共享内容只声明一次，降低维护成本，同时保证两端 ABI 对齐。
 *
 * 约束：
 *   - 所有对外类型保持 opaque（typedef 指向未定义的 struct *）；
 *   - 枚举值只追加、不修改、不复用；
 *   - 函数签名只增不改；
 *   - 共享函数（如 log_config_*、video_frame_*、string_free 等）在 Client 和 Service 库中
 *     都各自实现/导出一份，以保证两端独立部署时的自包含性。
 *
 * ============================================================================
 *                    线程模型（Threading Model）— 两端通用
 * ============================================================================
 *   - 回调统一在 SDK 内部工作线程触发，**不在业务调用线程**。
 *   - 同一 opaque 对象（connect_cb / stream_cb 等）上的全部回调在 SDK 内部
 *     保证串行分发；不同对象之间可能并发。
 *   - 回调内禁止：阻塞 I/O、长耗时业务、调用 init/uninit/connect/disconnect。
 *   - 回调内允许（白名单）：
 *       * 所有 getter（video_frame_get_* / stream_stats_get_* / ...）
 *       * retain / release
 *       * close_stream / destroy_stream
 *       * send_notice / reply_to_service_req / svc_service_reply / post_topic
 *       * log_set_level / log_set_callback
 *   - `userdata` 在对应 cb 对象 destroy 前保持有效；SDK 不接管其生命周期。
 *   - 回调 drain 保证：
 *       * `disconnect` 同步返回后，保证不再有属于该 connect 会话的**新**回调
 *         被派发；但已经进入 dispatch 队列的回调仍会执行，直至 uninit。
 *       * `uninit` 同步返回前，SDK **保证**内部工作线程已全部 join、所有回调
 *         已执行完毕（happens-before uninit 返回）。业务可在 uninit 之后安全
 *         释放所有 userdata 以及与回调共享的资源，不会产生 UAF。
 *       * `close_stream` / `destroy_stream` 返回后，属于该 stream 的新回调
 *         不再触发；已入队回调会在 uninit 前执行完毕。
 *
 * ============================================================================
 *                    所有权与生命周期（Ownership & Lifetime）— 两端通用
 * ============================================================================
 *   - xxx_create / xxx_destroy 严格配对。
 *   - 传入 SDK 的 opaque 配置对象（connect_info / connect_cb / stream_param /
 *     stream_cb 等）在对应 API（connect / open_stream / create_stream）返回
 *     RFLOW_OK 的瞬间，SDK 已完成内部拷贝，调用方可立即 destroy。
 *   - 传入 SDK 的 `const void *payload` / `const char *` 在 API 同步返回前由
 *     SDK 完成拷贝；API 返回后调用方可释放原 buffer（异步回包类 API 同理）。
 *   - SDK 通过 out 参数返回的 `char *` / `void *` 必须用 librflow_string_free /
 *     librflow_buffer_free 释放。
 *   - 回调入参的只读 opaque 句柄（video_frame_t / stream_stats_t）默认仅回调
 *     栈内有效；需跨栈/跨线程持有必须 retain，使用完 release。
 *
 * ============================================================================
 *                    容量与上限（Limits & Quotas）— 默认值
 * ============================================================================
 *   - 一个进程只能运行一份 Client 库（连接单一设备到服务端）。
 *   - 同一 connect 会话下同时打开的 stream 数量：≤ RFLOW_LIMIT_MAX_STREAMS。
 *   - 单条 notice / service_req payload：≤ RFLOW_LIMIT_MAX_PAYLOAD_BYTES。
 *   - 具体数值见下方 RFLOW_LIMIT_* 宏；次版本内稳定，主版本变更会写入发行说明。
**/

#ifndef __LIBRFLOW_COMMON_H__
#define __LIBRFLOW_COMMON_H__

#include "librflow_version.h"
#include "librflow_typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************
 *                                 错误码
 ******************************************************************************/
typedef int32_t rflow_err_t;

#define RFLOW_OK                              0

/* 通用错误 0x0xxx */
#define RFLOW_ERR_FAIL                        (-0x0001)  /* 未知失败 */
#define RFLOW_ERR_PARAM                       (-0x0002)  /* 非法参数 */
#define RFLOW_ERR_NOT_SUPPORT                 (-0x0003)  /* 不支持 */
#define RFLOW_ERR_STATE                       (-0x0004)  /* 当前状态不允许该操作 */
#define RFLOW_ERR_NO_MEM                      (-0x0005)  /* 内存分配失败 */
#define RFLOW_ERR_TIMEOUT                     (-0x0006)  /* 超时 */
#define RFLOW_ERR_BUSY                        (-0x0007)  /* 资源繁忙 */
#define RFLOW_ERR_NOT_FOUND                   (-0x0008)  /* 对象不存在 */
#define RFLOW_ERR_TRUNCATED                   (-0x0009)  /* 输出缓冲区过小 */

/* 连接错误 0x1xxx */
#define RFLOW_ERR_CONN_FAIL                   (-0x1001)
#define RFLOW_ERR_CONN_AUTH                   (-0x1002)
#define RFLOW_ERR_CONN_LICENSE                (-0x1003)
#define RFLOW_ERR_CONN_NETWORK                (-0x1004)
#define RFLOW_ERR_CONN_KICKED                 (-0x1005)  /* 被服务端踢出 */

/* 流错误 0x2xxx */
#define RFLOW_ERR_STREAM_NOT_EXIST            (-0x2001)
#define RFLOW_ERR_STREAM_ALREADY_OPEN         (-0x2002)
#define RFLOW_ERR_STREAM_CODEC_UNSUPP         (-0x2003)
#define RFLOW_ERR_STREAM_ENCODER_FAIL         (-0x2004)
#define RFLOW_ERR_STREAM_NO_SUBSCRIBER        (-0x2005)

/******************************************************************************
 *                                 容量常量
 * 次版本（minor）内稳定；主版本变更会写入发行说明。
 ******************************************************************************/
#define RFLOW_LIMIT_MAX_STREAMS               16          /* 单进程并发流上限 */
#define RFLOW_LIMIT_MAX_PAYLOAD_BYTES         (1u << 20)  /* 单条信令 payload 上限 1 MiB */
#define RFLOW_LIMIT_MAX_URL_LEN               1024
#define RFLOW_LIMIT_MAX_DEVICE_ID_LEN         128
#define RFLOW_LIMIT_MAX_SERVICE_ID_LEN        64

/** 未设置 device_id 时用于信令 stream_id（device_id:stream_index）的默认前缀 */
#define RFLOW_DEFAULT_DEVICE_ID               "demo_device"

/******************************************************************************
 *                                 枚举
 ******************************************************************************/

/* 日志等级 */
typedef enum {
    RFLOW_LOG_TRACE = 0,
    RFLOW_LOG_DEBUG = 1,
    RFLOW_LOG_INFO  = 2,
    RFLOW_LOG_WARN  = 3,
    RFLOW_LOG_ERROR = 4,
    RFLOW_LOG_FATAL = 5,
} rflow_log_level_t;

/* 视频编码 / 像素格式 */
typedef enum {
    RFLOW_CODEC_UNKNOWN = 0,

    /* 原始 YUV 族 */
    RFLOW_CODEC_I420    = 0x0001,
    RFLOW_CODEC_NV12    = 0x0002,
    RFLOW_CODEC_YUYV    = 0x0003,
    RFLOW_CODEC_UYVY    = 0x0004,

    /* 压缩族 */
    RFLOW_CODEC_H264    = 0x1001,
    RFLOW_CODEC_H265    = 0x1002,
    RFLOW_CODEC_MJPEG   = 0x1003,
} rflow_codec_t;

/* 视频帧类型 */
typedef enum {
    RFLOW_FRAME_UNKNOWN = 0,
    RFLOW_FRAME_I       = 1,
    RFLOW_FRAME_P       = 2,
    RFLOW_FRAME_B       = 3,
} rflow_frame_type_t;

typedef enum {
    RFLOW_VIDEO_FRAME_BACKEND_UNKNOWN         = 0,
    RFLOW_VIDEO_FRAME_BACKEND_CPU_PLANAR      = 1,
    RFLOW_VIDEO_FRAME_BACKEND_GPU_EXTERNAL    = 2,
    RFLOW_VIDEO_FRAME_BACKEND_HARDWARE_BUFFER = 3,
} rflow_video_frame_backend_t;

typedef enum {
    RFLOW_NATIVE_HANDLE_NONE                    = 0,
    RFLOW_NATIVE_HANDLE_ANDROID_OES_TEXTURE     = 1,
    RFLOW_NATIVE_HANDLE_ANDROID_HARDWARE_BUFFER = 2,
} rflow_native_handle_type_t;

typedef enum {
    RFLOW_VIDEO_OUTPUT_MODE_CPU_PLANAR = 0,
    RFLOW_VIDEO_OUTPUT_MODE_PREFER_GPU = 1,
    RFLOW_VIDEO_OUTPUT_MODE_REQUIRE_GPU = 2,
} rflow_video_output_mode_t;

/* 连接状态 */
typedef enum {
    RFLOW_CONN_IDLE         = 0,
    RFLOW_CONN_CONNECTING   = 1,
    RFLOW_CONN_CONNECTED    = 2,
    RFLOW_CONN_DISCONNECTED = 3,
    RFLOW_CONN_FAILED       = 4,
} rflow_connect_state_t;

/*
 * 流状态（Client / Service 共用，同名同义）：
 *   - IDLE
 *       流对象已存在，但当前未进行媒体收发。
 *       Service: create_stream 成功后、stop_stream 完成后处于此状态，可再次 start_stream。
 *       Client: 枚举保留为共享语义；公开拉流句柄通常不会长期停留在该状态。
 *
 *   - OPENING
 *       异步建流 / 启动中。句柄有效，可接受 close_stream / destroy_stream 取消流程。
 *
 *   - OPENED
 *       流已就绪并处于正常媒体收发状态。
 *
 *   - CLOSING
 *       异步关闭中。该状态只表示关闭流程已开始；之后只允许落到 CLOSED。
 *
 *   - CLOSED
 *       正常关闭终态。由 close_stream / destroy_stream / disconnect / uninit 等主动或受控清理触发。
 *       这是终态；SDK 不再为该 handle 派发新的 stream 回调。
 *
 *   - FAILED
 *       异常失败终态。由协商失败、链路故障、编解码异常等不可恢复错误触发。
 *       这是终态；SDK 不再为该 handle 派发新的 stream 回调。
 *
 * 统一约束：
 *   - CLOSED 与 FAILED 都是终态，语义不同但都表示该 handle 生命周期结束；
 *   - 不存在 FAILED -> CLOSED 的二次终态通知；调用方只会收到一个终态；
 *   - reason 仅在 FAILED 时表示失败原因；其余状态统一为 RFLOW_OK。
 *
 * 采集路径切换语义（同一逻辑流内）：
 *   - “切采集路径 / 切相机 / 切输入源”属于控制面动作，不属于流生命周期动作；
 *   - 若切换在同一逻辑 stream 内完成且媒体链路可持续复用，则不触发 stream_state
 *     跳变：不派发 OPENING / CLOSING / CLOSED，仅表现为媒体内容切换，handle 与 index
 *     保持不变；
 *   - 切换成功/失败的业务确认应通过控制面消息完成（notice / request-reply / topic），
 *     而不是复用 stream_state；
 *   - 只有当切换导致该逻辑流无法继续工作，且 SDK 无法在当前 handle 内恢复时，
 *     才派发 stream_state(FAILED, reason)。
 *
 * 预热缓冲语义（Service 内部实现约束）：
 *   - 预热缓冲仅适用于 Service 侧 stream 处于 OPENING 的窗口，用于隐藏 WebRTC
 *     建链、编码器预热、关键帧准备等启动时延；
 *   - 预热缓冲是每路 stream 的 SDK 内部有界实时队列，对调用方不可见，不引入额外状态；
 *   - 流从 OPENING 进入 OPENED 后，SDK 按实时策略发送预热缓冲中的可用帧；若部分帧因
 *     时效性被丢弃，仍不视为接口错误；
 *   - 流进入 IDLE / CLOSING / CLOSED / FAILED，或发生 stop / destroy / disconnect /
 *     uninit 时，未消费的预热缓冲全部丢弃；
 *   - Client 侧不存在对业务可见的“预热缓冲状态”；Client 收到 OPENED 仅表示接收链路
 *     已就绪，不保证首帧已到达。
 */
typedef enum {
    RFLOW_STREAM_IDLE     = 0,
    RFLOW_STREAM_OPENING  = 1,
    RFLOW_STREAM_OPENED   = 2,
    RFLOW_STREAM_CLOSING  = 3,
    RFLOW_STREAM_CLOSED   = 4,
    RFLOW_STREAM_FAILED   = 5,
} rflow_stream_state_t;

/* 信令服务器工作模式（Service 端常用；Client 端一般走默认 SIGNAL 模式） */
typedef enum {
    RFLOW_SIGNAL_MODE_SERVER  = 1,   /* 依赖外部信令服务器 */
    RFLOW_SIGNAL_MODE_DIRECT  = 2,   /* 设备端自己充当信令服务器 */
} rflow_signal_mode_t;

/* 码率控制类型（Service 端编码器参数） */
typedef enum {
    RFLOW_RC_CQP  = 0,
    RFLOW_RC_CBR  = 1,
    RFLOW_RC_VBR  = 2,
    RFLOW_RC_ACQP = 3,  /* CQP 基础上开启峰控 */
} rflow_rc_mode_t;

/* 地区 */
typedef enum {
    RFLOW_REGION_CN       = 0,
    RFLOW_REGION_OVERSEAS = 1,
} rflow_region_t;

/******************************************************************************
 *                        Opaque Handles — 共享配置 / 数据对象
 ******************************************************************************/

/* 共享配置：两端使用相同 ABI */
typedef struct librflow_log_config_s*      librflow_log_config_t;
typedef struct librflow_signal_config_s*   librflow_signal_config_t;
typedef struct librflow_license_config_s*  librflow_license_config_t;
typedef struct librflow_global_config_s*   librflow_global_config_t;

/* 共享只读数据对象（回调入参） */
typedef const struct librflow_video_frame_s*   librflow_video_frame_t;
typedef const struct librflow_stream_stats_s*  librflow_stream_stats_t;

/******************************************************************************
 *                        业务协议 ID 别名（Client / Service 共享）
 ******************************************************************************/
/*
 * 业务协议中的资源 ID：Client 侧 open_stream 与 Service 侧 create_stream 使用
 * 同一 index 空间，需一一对应。使用 typedef 别名以明确语义，未来可无痛收窄/扩展。
 * 约束：
 *   - 建议业务使用非负值；负值保留给 SDK 内部诊断，不保证未来不赋予特殊语义。
 *   - index 的具体取值范围由业务协议自定义，SDK 不校验上限。
 *   - 当前 v1 约定：仅支持主路 stream_idx = 0；若未来扩多路，再在保持兼容的前提下
 *     追加 1, 2, ... 的业务语义。
 */
typedef int32_t rflow_stream_index_t;
typedef int32_t rflow_notice_index_t;

/******************************************************************************
 *                           通用工具 API
 ******************************************************************************/

/* 内存释放：所有 SDK 返回的 char* / void* 必须调用对应 free */
LIBRFLOW_API_EXPORT void  librflow_string_free(char *p);
LIBRFLOW_API_EXPORT void  librflow_buffer_free(void *p);

/**
 * 错误 / 版本 / 构建信息。
 *
 * librflow_get_last_error():
 *   - thread-local；仅对**紧邻的上一次同步 API 调用**（在当前线程内返回非 OK 的）
 *     有意义，返回该调用附加的人类可读诊断字符串。
 *   - 不覆盖异步失败：通过 on_state / on_stream_state 的 `reason` 参数传递的
 *     错误**不会**写入 last_error，业务不应在回调内依赖此函数诊断异步原因。
 *   - 在未产生错误的线程调用返回 ""（空串）或上一次陈旧值，不可作为错误发生的
 *     判定依据。永远以 API 返回的 rflow_err_t 为准。
 *   - 返回字符串生命周期持续到当前线程下一次进入任意 librflow_* API 为止。
 */
LIBRFLOW_API_EXPORT const char* librflow_get_last_error(void);
LIBRFLOW_API_EXPORT void        librflow_get_version(uint32_t *major, uint32_t *minor, uint32_t *patch);
LIBRFLOW_API_EXPORT const char* librflow_get_build_info(void);

/**
 * 错误码 → 静态字符串名（如 "RFLOW_ERR_PARAM"），用于日志。
 * 返回值为静态字符串，调用方不得 free。未知错误码返回 "RFLOW_ERR_UNKNOWN"。
 */
LIBRFLOW_API_EXPORT const char* librflow_err_to_string(rflow_err_t err);

/******************************************************************************
 *                           LogConfig（共享）
 ******************************************************************************/
typedef void (*librflow_log_cb_fn)(rflow_log_level_t level,
                                   const char *msg,
                                   void *userdata);

LIBRFLOW_API_EXPORT librflow_log_config_t librflow_log_config_create(void);
LIBRFLOW_API_EXPORT void                  librflow_log_config_destroy(librflow_log_config_t cfg);

LIBRFLOW_API_EXPORT rflow_err_t librflow_log_config_set_level   (librflow_log_config_t cfg, rflow_log_level_t level);
LIBRFLOW_API_EXPORT rflow_err_t librflow_log_config_set_enable  (librflow_log_config_t cfg, bool enable);
LIBRFLOW_API_EXPORT rflow_err_t librflow_log_config_set_callback(librflow_log_config_t cfg,
                                                                 librflow_log_cb_fn cb,
                                                                 void *userdata);
LIBRFLOW_API_EXPORT rflow_log_level_t librflow_log_config_get_level (librflow_log_config_t cfg);
LIBRFLOW_API_EXPORT bool              librflow_log_config_get_enable(librflow_log_config_t cfg);

/******************************************************************************
 *                           SignalConfig（共享）
 ******************************************************************************/
LIBRFLOW_API_EXPORT librflow_signal_config_t librflow_signal_config_create(void);
LIBRFLOW_API_EXPORT void                     librflow_signal_config_destroy(librflow_signal_config_t cfg);

LIBRFLOW_API_EXPORT rflow_err_t librflow_signal_config_set_url        (librflow_signal_config_t cfg, const char *url);
/* 仅 Service 端可能用到；Client 端默认 SERVER 模式，不需调用 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_signal_config_set_mode       (librflow_signal_config_t cfg, rflow_signal_mode_t mode);
LIBRFLOW_API_EXPORT rflow_err_t librflow_signal_config_set_direct_port(librflow_signal_config_t cfg, uint16_t port);
LIBRFLOW_API_EXPORT rflow_err_t librflow_signal_config_set_connect_timeout_ms(librflow_signal_config_t cfg,
                                                                               uint32_t timeout_ms);
LIBRFLOW_API_EXPORT rflow_err_t librflow_signal_config_set_keepalive_interval_ms(librflow_signal_config_t cfg,
                                                                                  uint32_t interval_ms);
LIBRFLOW_API_EXPORT rflow_err_t librflow_signal_config_set_reconnect_enable(librflow_signal_config_t cfg,
                                                                             bool enable);
LIBRFLOW_API_EXPORT rflow_err_t librflow_signal_config_set_reconnect_backoff_ms(librflow_signal_config_t cfg,
                                                                                 uint32_t initial_backoff_ms,
                                                                                 uint32_t max_backoff_ms);
/* 0 表示不限重试次数 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_signal_config_set_reconnect_max_attempts(librflow_signal_config_t cfg,
                                                                                   uint32_t max_attempts);

/*
 * Getter：写入 buf（含终止 \0）。
 *   - buf == NULL 或 buf_len == 0：仅探测，out_needed 返回所需长度（含 \0）；
 *   - buf_len 不足：返回 RFLOW_ERR_TRUNCATED，out_needed 返回所需长度；
 *   - 字段未设置：返回 RFLOW_ERR_NOT_FOUND。
 * out_needed 可为 NULL。
 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_signal_config_get_url(librflow_signal_config_t cfg,
                                                               char *buf, uint32_t buf_len,
                                                               uint32_t *out_needed);

/******************************************************************************
 *                           LicenseConfig（共享）
 ******************************************************************************/
LIBRFLOW_API_EXPORT librflow_license_config_t librflow_license_config_create(void);
LIBRFLOW_API_EXPORT void                      librflow_license_config_destroy(librflow_license_config_t cfg);

LIBRFLOW_API_EXPORT rflow_err_t librflow_license_config_set_file  (librflow_license_config_t cfg, const char *path);
LIBRFLOW_API_EXPORT rflow_err_t librflow_license_config_set_buffer(librflow_license_config_t cfg,
                                                                   const void *data, uint32_t len);

/*
 * 读回当前配置的 license 文件路径；若通过 set_buffer 注入则返回 RFLOW_ERR_NOT_FOUND。
 * 探测语义与 librflow_connect_info_get_device_id 一致（见该函数注释）。
 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_license_config_get_file(librflow_license_config_t cfg,
                                                                  char *buf, uint32_t buf_len,
                                                                  uint32_t *out_needed);

/******************************************************************************
 *                           GlobalConfig（共享）
 ******************************************************************************/
LIBRFLOW_API_EXPORT librflow_global_config_t librflow_global_config_create(void);
LIBRFLOW_API_EXPORT void                     librflow_global_config_destroy(librflow_global_config_t cfg);

LIBRFLOW_API_EXPORT rflow_err_t librflow_global_config_set_log        (librflow_global_config_t cfg, librflow_log_config_t log);
LIBRFLOW_API_EXPORT rflow_err_t librflow_global_config_set_signal     (librflow_global_config_t cfg, librflow_signal_config_t sig);
LIBRFLOW_API_EXPORT rflow_err_t librflow_global_config_set_license    (librflow_global_config_t cfg, librflow_license_config_t lic);
/* 可读写的配置文件存储目录；NULL 使用默认路径 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_global_config_set_config_path(librflow_global_config_t cfg, const char *path);
LIBRFLOW_API_EXPORT rflow_err_t librflow_global_config_set_region     (librflow_global_config_t cfg, rflow_region_t region);

/******************************************************************************
 *                           Video Frame Getter / 生命周期（共享）
 ******************************************************************************/
/*
 * 视频帧格式说明：
 *   - codec == RFLOW_CODEC_I420:
 *       plane_count = 3
 *       plane 0 = Y, plane 1 = U, plane 2 = V
 *   - codec == RFLOW_CODEC_NV12:
 *       plane_count = 2
 *       plane 0 = Y, plane 1 = UV(交错)
 *
 * 推荐用法：
 *   1. 先读 codec / width / height / plane_count
 *   2. 再按 plane 读取 data/stride/plane_width/plane_height
 *   3. 仅在你明确需要“单块连续内存”时，才使用 get_data/get_data_size
 *
 * 兼容说明：
 *   - get_data/get_data_size 仍保留给旧代码。
 *   - 当帧底层是多平面 buffer 时，SDK 会在首次调用 get_data 时按 codec
 *     规则拼出连续缓存；新代码优先使用 plane getter，避免额外拷贝。
 */
/*
 * GPU/native texture prepare hook.
 * If present, call it on the target GL thread after acquire_for_sampling()
 * and before sampling the exported texture handle.
 */
typedef void (*librflow_gl_prepare_fn)(void *userdata);

/*
 * 线程安全（Video Frame Getter）
 * ------------------------------
 *   - 所有 librflow_video_frame_get_* 在同一帧上**允许多线程并发只读调用**：
 *       * 帧的元数据字段（width/height/codec/type/seq/pts_ms/utc_ms/index/
 *         plane_count/plane_data/plane_strides/plane_widths/plane_heights/
 *         backend/native_handle_type/oes_texture_id/android_hardware_buffer/
 *         sync_fence_fd/gl_prepare_callback）在帧构造完成后即不可变，纯只读。
 *       * librflow_video_frame_get_data / get_data_size 内部使用互斥锁懒生成
 *         连续 payload，多线程同时首次访问也安全（最多串行化一次 memcpy）。
 *   - 前提：调用方已通过 retain 获得对应引用，并且未被对应的 release 释放。
 *     SDK 不会针对“已 release 的悬空指针 + 并发 getter”做防御。
 *   - librflow_video_frame_acquire_for_sampling /
 *     librflow_video_frame_release_after_sampling 的并发性由具体 backend 实现
 *     约定；CPU planar 帧为 no-op，安全。
 */
LIBRFLOW_API_EXPORT rflow_video_frame_backend_t librflow_video_frame_get_backend(librflow_video_frame_t f);
LIBRFLOW_API_EXPORT rflow_native_handle_type_t  librflow_video_frame_get_native_handle_type(librflow_video_frame_t f);
/* Returns 0 when this frame is not backed by Android OES texture. */
LIBRFLOW_API_EXPORT uint32_t           librflow_video_frame_get_oes_texture_id(librflow_video_frame_t f);
/* Returns NULL when this frame is not backed by Android AHardwareBuffer. The pointer is borrowed from `frame`. */
LIBRFLOW_API_EXPORT void*              librflow_video_frame_get_android_hardware_buffer(librflow_video_frame_t f);
/* Returns -1 when there is no sync fence. The fd is borrowed from `frame` and must not be closed by caller. */
LIBRFLOW_API_EXPORT int32_t            librflow_video_frame_get_sync_fence_fd(librflow_video_frame_t f);
/* Returns RFLOW_ERR_NOT_SUPPORT when the current frame backend has no prepare hook. */
LIBRFLOW_API_EXPORT rflow_err_t        librflow_video_frame_get_gl_prepare_callback(
                                            librflow_video_frame_t f,
                                            librflow_gl_prepare_fn *out_fn,
                                            void **out_userdata);
/* Acquire sampling rights for GPU/native texture backends. CPU planar frames return RFLOW_ERR_NOT_SUPPORT. */
LIBRFLOW_API_EXPORT rflow_err_t        librflow_video_frame_acquire_for_sampling(librflow_video_frame_t f);
/* Paired with acquire_for_sampling(); CPU planar frames are a no-op. */
LIBRFLOW_API_EXPORT void               librflow_video_frame_release_after_sampling(librflow_video_frame_t f);
LIBRFLOW_API_EXPORT rflow_codec_t      librflow_video_frame_get_codec    (librflow_video_frame_t f);
LIBRFLOW_API_EXPORT rflow_frame_type_t librflow_video_frame_get_type     (librflow_video_frame_t f);
LIBRFLOW_API_EXPORT uint32_t           librflow_video_frame_get_plane_count (librflow_video_frame_t f);
LIBRFLOW_API_EXPORT const uint8_t*     librflow_video_frame_get_plane_data  (librflow_video_frame_t f, uint32_t plane_index);
LIBRFLOW_API_EXPORT uint32_t           librflow_video_frame_get_plane_stride(librflow_video_frame_t f, uint32_t plane_index);
LIBRFLOW_API_EXPORT uint32_t           librflow_video_frame_get_plane_width (librflow_video_frame_t f, uint32_t plane_index);
LIBRFLOW_API_EXPORT uint32_t           librflow_video_frame_get_plane_height(librflow_video_frame_t f, uint32_t plane_index);
/* 旧接口：需要连续内存时再用；新代码优先使用 plane getter。 */
LIBRFLOW_API_EXPORT const uint8_t*     librflow_video_frame_get_data     (librflow_video_frame_t f);
LIBRFLOW_API_EXPORT uint32_t           librflow_video_frame_get_data_size(librflow_video_frame_t f);
LIBRFLOW_API_EXPORT uint32_t           librflow_video_frame_get_width    (librflow_video_frame_t f);
LIBRFLOW_API_EXPORT uint32_t           librflow_video_frame_get_height   (librflow_video_frame_t f);
LIBRFLOW_API_EXPORT uint64_t           librflow_video_frame_get_pts_ms   (librflow_video_frame_t f);
LIBRFLOW_API_EXPORT uint64_t           librflow_video_frame_get_utc_ms   (librflow_video_frame_t f);
LIBRFLOW_API_EXPORT uint32_t           librflow_video_frame_get_seq      (librflow_video_frame_t f);
LIBRFLOW_API_EXPORT int32_t            librflow_video_frame_get_index    (librflow_video_frame_t f);

/*
 * 线程安全（retain / release）
 * ----------------------------
 *   - librflow_video_frame_retain / librflow_video_frame_release **允许在任意
 *     线程调用**。引用计数为原子操作（retain 使用 relaxed，release 使用
 *     acq_rel），与标准 shared_ptr 引用计数模式一致。
 *   - 每个调用方各自管理“自己持有的那一份引用”：retain 后必须由“持有者”
 *     在使用完毕时调用一次 release，可以是不同线程（例如 SDK 回调线程
 *     retain 并转交，渲染/GUI 线程在 mailbox 被覆盖时 release）。
 *   - 当最后一次 release（fetch_sub 后引用计数变为 0）时，**同步**释放底层
 *     资源（包括 webrtc 帧 buffer 引用、Android AHardwareBuffer 引用、
 *     sync fence fd 等）；执行 release 的线程承担释放成本。
 *   - 不要将“同一份引用”跨线程裸传共享后又分别 release——这等于双重
 *     释放，是错误用法（与 shared_ptr 的复制规则一致）。
 */
LIBRFLOW_API_EXPORT librflow_video_frame_t librflow_video_frame_retain (librflow_video_frame_t f);
LIBRFLOW_API_EXPORT void                   librflow_video_frame_release(librflow_video_frame_t f);

/******************************************************************************
 *                           Stream Stats Getter（共享）
 ******************************************************************************/
/*
 * stream_stats 默认仅在当前回调栈或当前 API 调用返回前有效。
 * 如需跨栈/跨线程持有，必须 retain；使用完成后 release。
 */
LIBRFLOW_API_EXPORT librflow_stream_stats_t librflow_stream_stats_retain (librflow_stream_stats_t s);
LIBRFLOW_API_EXPORT void                    librflow_stream_stats_release(librflow_stream_stats_t s);

LIBRFLOW_API_EXPORT uint32_t librflow_stream_stats_get_duration_ms    (librflow_stream_stats_t s);
LIBRFLOW_API_EXPORT uint64_t librflow_stream_stats_get_in_bound_bytes (librflow_stream_stats_t s);
LIBRFLOW_API_EXPORT uint64_t librflow_stream_stats_get_in_bound_pkts  (librflow_stream_stats_t s);
LIBRFLOW_API_EXPORT uint64_t librflow_stream_stats_get_out_bound_bytes(librflow_stream_stats_t s);
LIBRFLOW_API_EXPORT uint64_t librflow_stream_stats_get_out_bound_pkts (librflow_stream_stats_t s);
LIBRFLOW_API_EXPORT uint32_t librflow_stream_stats_get_lost_pkts      (librflow_stream_stats_t s);
LIBRFLOW_API_EXPORT uint32_t librflow_stream_stats_get_bitrate_kbps   (librflow_stream_stats_t s);
LIBRFLOW_API_EXPORT uint32_t librflow_stream_stats_get_rtt_ms         (librflow_stream_stats_t s);
LIBRFLOW_API_EXPORT uint32_t librflow_stream_stats_get_fps            (librflow_stream_stats_t s);

/* QoS 补充指标（Client 拉流更关心） */
LIBRFLOW_API_EXPORT uint32_t librflow_stream_stats_get_jitter_ms          (librflow_stream_stats_t s);
LIBRFLOW_API_EXPORT uint32_t librflow_stream_stats_get_freeze_count       (librflow_stream_stats_t s);
LIBRFLOW_API_EXPORT uint32_t librflow_stream_stats_get_decode_fail_count  (librflow_stream_stats_t s);

/*
 * 抖动缓存平均延迟（jb_avg）：来自 RTCInboundRtpStreamStats::jitter_buffer_delay
 * 与 jitter_buffer_emitted_count 的比值，单位 ms。代表"帧从入抖动缓存到出缓存"
 * 的平均等待时间，是判断网络抖动 + jitter buffer 适配情况的核心指标。
 */
LIBRFLOW_API_EXPORT uint32_t librflow_stream_stats_get_jitter_buffer_delay_ms(librflow_stream_stats_t s);
/*
 * 当前 RtpReceiver 上的 min playout delay（playout 时序的下限），单位 ms。
 * 由 SDK 内部 jitter min-delay 控制（关键帧 watchdog 触发后会临时上调，再回归
 * floor）。多数场景为常量；用于上层显示"播放下限"。
 */
LIBRFLOW_API_EXPORT uint32_t librflow_stream_stats_get_jitter_min_delay_ms  (librflow_stream_stats_t s);

#ifdef __cplusplus
}
#endif

#endif /* __LIBRFLOW_COMMON_H__ */
