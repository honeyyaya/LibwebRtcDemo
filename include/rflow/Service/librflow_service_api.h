/**
 * @file     librflow_service_api.h
 * @brief    Service 端对外 API（纯 C ABI，opaque handle + get/set）
 *
 * Service 角色：设备侧媒体发布端，内部封装 WebRTC 和编码器。
 *   - 业务层通过 push_video_frame 将采集数据送入 SDK（音频暂不支持）；
 *     SDK 按 stream_param 指定的目标编码格式进行编码/透传/转码，并推送给订阅端；
 *   - 处理 Client 侧的业务服务请求：on_service_req + librflow_svc_service_reply
 *     （Client 侧应答为 librflow_reply_to_service_req）；
 *   - 处理 Client 侧的订阅行为：on_pull_request / on_pull_release 通知业务层
 *     某路流何时需要开始/停止产生数据。
 *
 * 共享部分（log/signal/license/global config、video_frame、stream_stats 的 getter、
 * 内存释放、错误与版本、线程模型与所有权约定）见 librflow_common.h 顶部注释。
 *
 * 调用时序：
 *   set_global_config -> svc_init -> svc_connect ->
 *     svc_create_stream -> svc_start_stream -> [push_video_frame]*
 *       -> svc_stop_stream -> svc_destroy_stream
 *   -> svc_disconnect -> svc_uninit
 *   svc_disconnect / svc_uninit / svc_destroy_stream 均幂等强清理。
 *
 * ============================================================================
 *                   Service 端线程与调用约束（通用规则见 common.h）
 * ============================================================================
 *   - 同一 svc_connect_cb 上的 on_state / on_bind_state / on_notice /
 *     on_service_req / on_pull_request / on_pull_release 串行分发。
 *   - 同一 svc_stream_cb 上的 on_state / on_encoded_video / on_stream_stats
 *     串行分发；不同 svc_stream_cb 之间可能并发。
 *   - on_encoded_video 与 on_stream_stats 可能由不同线程分发；共享业务状态需自行加锁。
 *   - svc_set_global_config / svc_init / svc_uninit / svc_connect / svc_disconnect
 *     必须在**同一业务线程**串行调用，且不得在任何回调内调用。
 *   - svc_create_stream / svc_start_stream / svc_stop_stream / svc_destroy_stream /
 *     svc_push_video_frame / svc_stream_set_bitrate / svc_stream_get_stats /
 *     svc_send_notice / svc_post_topic / svc_service_reply / svc_log_set_*
 *     线程安全，可在回调内调用（svc_destroy_stream 见 common.h 白名单）。
**/

#ifndef __LIBRFLOW_SERVICE_API_H__
#define __LIBRFLOW_SERVICE_API_H__

#include "../librflow_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************
 *                          Service 专属枚举
 ******************************************************************************/

/* 设备绑定状态 */
typedef enum {
    RFLOW_BIND_UNBOUND   = 0,
    RFLOW_BIND_BOUND     = 1,
    RFLOW_BIND_FAILED    = 2,
} rflow_bind_state_t;

/******************************************************************************
 *                          Opaque Handles — Service 专属
 ******************************************************************************/
typedef struct librflow_svc_connect_info_s*  librflow_svc_connect_info_t;
typedef struct librflow_svc_connect_cb_s*    librflow_svc_connect_cb_t;
typedef struct librflow_svc_stream_param_s*  librflow_svc_stream_param_t;
typedef struct librflow_svc_stream_cb_s*     librflow_svc_stream_cb_t;
typedef struct librflow_svc_stream_s*        librflow_svc_stream_handle_t;
typedef struct librflow_svc_push_frame_s*    librflow_svc_push_frame_t;   /* 业务向 SDK 推送的帧 */
typedef struct librflow_svc_license_info_s*  librflow_svc_license_info_t; /* license 信息只读对象 */

/******************************************************************************
 *                          ConnectInfo（设备身份 + 产品信息）
 ******************************************************************************/
LIBRFLOW_API_EXPORT librflow_svc_connect_info_t librflow_svc_connect_info_create(void);
LIBRFLOW_API_EXPORT void                        librflow_svc_connect_info_destroy(librflow_svc_connect_info_t info);

LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_connect_info_set_device_id    (librflow_svc_connect_info_t info, const char *device_id);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_connect_info_set_device_secret(librflow_svc_connect_info_t info, const char *device_secret);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_connect_info_set_product_key  (librflow_svc_connect_info_t info, const char *product_key);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_connect_info_set_vendor_id    (librflow_svc_connect_info_t info, const char *vid);

/*
 * Getter：写入 buf（含终止 \0）。探测语义与 Client 侧
 * librflow_connect_info_get_device_id 一致：
 *   - buf == NULL 或 buf_len == 0：仅探测，out_needed 返回所需长度（含 \0），返回 RFLOW_ERR_TRUNCATED；
 *   - buf_len 不足：返回 RFLOW_ERR_TRUNCATED，out_needed 返回所需长度；
 *   - 字段未设置：返回 RFLOW_ERR_NOT_FOUND。
 * out_needed 可为 NULL。
 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_connect_info_get_device_id    (librflow_svc_connect_info_t info,
                                                                             char *buf, uint32_t buf_len,
                                                                             uint32_t *out_needed);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_connect_info_get_device_secret(librflow_svc_connect_info_t info,
                                                                             char *buf, uint32_t buf_len,
                                                                             uint32_t *out_needed);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_connect_info_get_product_key  (librflow_svc_connect_info_t info,
                                                                             char *buf, uint32_t buf_len,
                                                                             uint32_t *out_needed);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_connect_info_get_vendor_id    (librflow_svc_connect_info_t info,
                                                                             char *buf, uint32_t buf_len,
                                                                             uint32_t *out_needed);

/******************************************************************************
 *                          ConnectCb
 ******************************************************************************/

/**
 * 连接状态变化
 * @param reason 若 state = FAILED/DISCONNECTED，为原因错误码；否则 RFLOW_OK
 */
typedef void (*librflow_svc_on_connect_state_fn)(rflow_connect_state_t state,
                                                 rflow_err_t reason,
                                                 void *userdata);

/** 设备绑定状态变化；extra 为可选文本（失败原因等），仅回调栈内有效 */
typedef void (*librflow_svc_on_bind_state_fn)(rflow_bind_state_t state,
                                              const char *extra,
                                              void *userdata);

/**
 * 通知类消息（来自 Client，单向，不要求回包）
 * @param payload 仅回调栈内有效；如需跨栈持有，业务层自行深拷贝。
 *
 * 约定：若业务将某个 notice_index 用作“切采集路径 / 切相机”控制命令，则该命令的成功
 * 与失败应通过控制面消息确认，不应借由 on_stream_state 做“伪重连/伪切流”通知。
 */
typedef void (*librflow_svc_on_notice_fn)(rflow_notice_index_t index,
                                          const void *payload,
                                          uint32_t len,
                                          void *userdata);

/**
 * 业务请求（来自 Client，需要回包）
 * @param req_id      SDK 分配的请求 ID，用于 librflow_svc_service_reply 异步回包
 * @param service_id  业务标识；仅回调栈内有效（建议即时拷贝到业务线程）
 * @param payload     业务数据；仅回调栈内有效
 * 超时未回包时，SDK 自动以 RFLOW_ERR_TIMEOUT 应答并释放 req_id。
 * 业务可在任意线程通过 req_id 回包；req_id 仅一次有效，二次回包返回
 * RFLOW_ERR_NOT_FOUND。
 */
typedef void (*librflow_svc_on_service_req_fn)(uint64_t req_id,
                                               const char *service_id,
                                               const void *payload,
                                               uint32_t len,
                                               void *userdata);

/**
 * Client 侧请求订阅某路流（pull）
 * SDK 触发此回调后，业务层应当保证 stream_idx 对应的流已 svc_create_stream
 * + svc_start_stream，并开始 push_video_frame。
 * 返回 void，业务处理失败通过 svc_stream_cb 的 on_state 回调体现（不回帧即可）。
 */
typedef void (*librflow_svc_on_pull_request_fn)(rflow_stream_index_t stream_idx,
                                                void *userdata);

/** Client 侧不再订阅该路流（最后一个订阅者离开） */
typedef void (*librflow_svc_on_pull_release_fn)(rflow_stream_index_t stream_idx,
                                                void *userdata);

LIBRFLOW_API_EXPORT librflow_svc_connect_cb_t librflow_svc_connect_cb_create(void);
LIBRFLOW_API_EXPORT void                      librflow_svc_connect_cb_destroy(librflow_svc_connect_cb_t cb);

LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_connect_cb_set_on_state        (librflow_svc_connect_cb_t cb, librflow_svc_on_connect_state_fn fn);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_connect_cb_set_on_bind_state   (librflow_svc_connect_cb_t cb, librflow_svc_on_bind_state_fn fn);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_connect_cb_set_on_notice       (librflow_svc_connect_cb_t cb, librflow_svc_on_notice_fn fn);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_connect_cb_set_on_service_req  (librflow_svc_connect_cb_t cb, librflow_svc_on_service_req_fn fn);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_connect_cb_set_on_pull_request (librflow_svc_connect_cb_t cb, librflow_svc_on_pull_request_fn fn);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_connect_cb_set_on_pull_release (librflow_svc_connect_cb_t cb, librflow_svc_on_pull_release_fn fn);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_connect_cb_set_userdata        (librflow_svc_connect_cb_t cb, void *userdata);

/******************************************************************************
 *                          StreamParam（下行编码流参数）
 ******************************************************************************/
LIBRFLOW_API_EXPORT librflow_svc_stream_param_t librflow_svc_stream_param_create(void);
LIBRFLOW_API_EXPORT void                        librflow_svc_stream_param_destroy(librflow_svc_stream_param_t p);

/* 输入视频编码：业务 push 进来的原始或已编码帧类型 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_param_set_in_codec (librflow_svc_stream_param_t p, rflow_codec_t codec);
/* 输出视频编码：SDK 发布给订阅端的编码类型；与输入相同且未开启 transcode 时透传 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_param_set_out_codec(librflow_svc_stream_param_t p, rflow_codec_t codec);
/* 输入分辨率（源） */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_param_set_src_size (librflow_svc_stream_param_t p, uint32_t width, uint32_t height);
/* 输出分辨率 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_param_set_out_size (librflow_svc_stream_param_t p, uint32_t width, uint32_t height);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_param_set_fps      (librflow_svc_stream_param_t p, uint32_t fps);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_param_set_gop      (librflow_svc_stream_param_t p, uint32_t gop_size);

/* 码率控制 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_param_set_rc_mode  (librflow_svc_stream_param_t p, rflow_rc_mode_t rc);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_param_set_qp       (librflow_svc_stream_param_t p, uint32_t qp);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_param_set_bitrate  (librflow_svc_stream_param_t p, uint32_t bitrate_kbps, uint32_t max_bitrate_kbps);

/* 动态码率：启用时按 [min, max] 动态调整 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_param_set_dynamic_bitrate(librflow_svc_stream_param_t p,
                                                                              bool     enable,
                                                                              uint32_t lowest_kbps,
                                                                              uint32_t highest_kbps);

/* 输入/输出 codec 相同且不启用，SDK 透传；启用时即使相同 codec 也会强制转码 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_param_set_enable_transcode(librflow_svc_stream_param_t p, bool enable);
/*
 * 鍐呴儴閲囬泦锛氳缃?video_device_path 鎴?video_device_index 鍚庯紝SDK 浣跨敤鍐呴儴鎽勫儚澶撮噰闆?
 * + 缂栫爜 + 鎺ㄦ祦锛屼笟鍔″眰涓嶅簲鍐嶈皟鐢?librflow_svc_push_video_frame 鍚戣娴佹姇甯с€?
 * video_device_path 浼樺厛浜?video_device_index锛涗袱鑰呭潎鏈缃椂淇濇寔澶栭儴 push 妯″紡銆? */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_param_set_video_device_path (librflow_svc_stream_param_t p, const char *device_path);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_param_set_video_device_index(librflow_svc_stream_param_t p, uint32_t device_index);

/*
 * Getter：读回当前设置值。统一返回 rflow_err_t，以消除"未设置"与"显式设为 0 /
 * UNKNOWN"的语义歧义：
 *   - 字段未被 setter 赋值过：返回 RFLOW_ERR_NOT_FOUND，out 不被写入；
 *   - 字段已被显式设置（包括 0）：返回 RFLOW_OK，值写入 out。
 * out 指针不得为 NULL（违反返回 RFLOW_ERR_PARAM）。
 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_param_get_in_codec  (librflow_svc_stream_param_t p, rflow_codec_t *out_codec);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_param_get_out_codec (librflow_svc_stream_param_t p, rflow_codec_t *out_codec);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_param_get_src_size  (librflow_svc_stream_param_t p,
                                                                         uint32_t *out_width, uint32_t *out_height);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_param_get_out_size  (librflow_svc_stream_param_t p,
                                                                         uint32_t *out_width, uint32_t *out_height);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_param_get_fps       (librflow_svc_stream_param_t p, uint32_t *out_fps);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_param_get_gop       (librflow_svc_stream_param_t p, uint32_t *out_gop);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_param_get_rc_mode   (librflow_svc_stream_param_t p, rflow_rc_mode_t *out_rc);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_param_get_qp        (librflow_svc_stream_param_t p, uint32_t *out_qp);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_param_get_bitrate   (librflow_svc_stream_param_t p,
                                                                         uint32_t *out_bitrate_kbps,
                                                                         uint32_t *out_max_bitrate_kbps);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_param_get_dynamic_bitrate(librflow_svc_stream_param_t p,
                                                                              bool     *out_enable,
                                                                              uint32_t *out_lowest_kbps,
                                                                              uint32_t *out_highest_kbps);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_param_get_enable_transcode(librflow_svc_stream_param_t p, bool *out_enable);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_param_get_video_device_path (librflow_svc_stream_param_t p,
                                                                                 char *buf, uint32_t buf_len,
                                                                                 uint32_t *out_needed);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_param_get_video_device_index(librflow_svc_stream_param_t p,
                                                                                 uint32_t *out_device_index);

/******************************************************************************
 *                          StreamCb（流本地观测回调，可选）
 ******************************************************************************/

/*
 * 流状态变化；状态名语义以 librflow_common.h 中 rflow_stream_state_t 注释为准。
 *   - Service 正常启动路径：IDLE -> OPENING -> OPENED
 *   - Service 正常停止路径：OPENED -> CLOSING -> IDLE
 *   - Service 销毁路径：IDLE / OPENING / OPENED -> CLOSING -> CLOSED
 *   - Service 异常路径：OPENING / OPENED -> FAILED
 *   - CLOSED 与 FAILED 均为终态，且二者二选一；不会先 FAILED 再 CLOSED
 *   - reason 仅在 state == RFLOW_STREAM_FAILED 时为失败原因；其余状态均为 RFLOW_OK
 *   - 收到终态回调后 handle 失效，不得再传入任何 API
 */
typedef void (*librflow_svc_on_stream_state_fn)(librflow_svc_stream_handle_t handle,
                                                rflow_stream_state_t state,
                                                rflow_err_t reason,
                                                void *userdata);

/** 编码后视频帧（本地观测用，例如落盘/录像）；frame 仅回调栈内有效 */
typedef void (*librflow_svc_on_encoded_video_fn)(librflow_svc_stream_handle_t handle,
                                                 librflow_video_frame_t frame,
                                                 void *userdata);

/**
 * 下行流统计（SDK 周期上报）
 * @param handle  触发统计回调的流句柄，避免多路发布时归属歧义
 * @param stats   默认仅回调栈内有效；如需跨栈/跨线程持有，使用
 *                librflow_stream_stats_retain / librflow_stream_stats_release
 */
typedef void (*librflow_svc_on_stream_stats_fn)(librflow_svc_stream_handle_t handle,
                                                librflow_stream_stats_t stats,
                                                void *userdata);

LIBRFLOW_API_EXPORT librflow_svc_stream_cb_t librflow_svc_stream_cb_create(void);
LIBRFLOW_API_EXPORT void                     librflow_svc_stream_cb_destroy(librflow_svc_stream_cb_t cb);

LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_cb_set_on_state         (librflow_svc_stream_cb_t cb, librflow_svc_on_stream_state_fn fn);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_cb_set_on_encoded_video (librflow_svc_stream_cb_t cb, librflow_svc_on_encoded_video_fn fn);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_cb_set_on_stream_stats (librflow_svc_stream_cb_t cb, librflow_svc_on_stream_stats_fn fn);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_cb_set_userdata         (librflow_svc_stream_cb_t cb, void *userdata);

/******************************************************************************
 *                          PushFrame（业务 → SDK 推帧对象，避免多参函数）
 ******************************************************************************/
LIBRFLOW_API_EXPORT librflow_svc_push_frame_t librflow_svc_push_frame_create(void);
LIBRFLOW_API_EXPORT void                      librflow_svc_push_frame_destroy(librflow_svc_push_frame_t f);

/* 必填 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_push_frame_set_codec   (librflow_svc_push_frame_t f, rflow_codec_t codec);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_push_frame_set_type    (librflow_svc_push_frame_t f, rflow_frame_type_t type);
/* SDK 内部 copy data，调用返回后业务可立即释放 buffer */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_push_frame_set_data    (librflow_svc_push_frame_t f,
                                                                      const void *data, uint32_t size);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_push_frame_set_size    (librflow_svc_push_frame_t f, uint32_t width, uint32_t height);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_push_frame_set_pts_ms  (librflow_svc_push_frame_t f, uint64_t pts_ms);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_push_frame_set_utc_ms  (librflow_svc_push_frame_t f, uint64_t utc_ms);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_push_frame_set_seq     (librflow_svc_push_frame_t f, uint32_t seq);

/* 可选：分片推送（大帧切块）；flush = true 表示当前帧已经推完 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_push_frame_set_flush   (librflow_svc_push_frame_t f, bool flush);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_push_frame_set_offset  (librflow_svc_push_frame_t f, uint32_t offset);

/*
 * Getter：读回当前设置值。语义与 svc_stream_param_get_* 一致：
 *   - 未设置字段：返回 RFLOW_ERR_NOT_FOUND；
 *   - 已显式设置：返回 RFLOW_OK；
 *   - out 为 NULL：返回 RFLOW_ERR_PARAM。
 *
 * 注意：get_data 返回的是 SDK 内部缓冲区的只读视图，生命周期与 push_frame
 * 对象一致（destroy 后失效）；仅供业务在 push 前自检/调试使用，不得跨线程持有。
 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_push_frame_get_codec   (librflow_svc_push_frame_t f, rflow_codec_t *out_codec);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_push_frame_get_type    (librflow_svc_push_frame_t f, rflow_frame_type_t *out_type);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_push_frame_get_data    (librflow_svc_push_frame_t f,
                                                                      const void **out_data, uint32_t *out_size);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_push_frame_get_size    (librflow_svc_push_frame_t f,
                                                                      uint32_t *out_width, uint32_t *out_height);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_push_frame_get_pts_ms  (librflow_svc_push_frame_t f, uint64_t *out_pts_ms);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_push_frame_get_utc_ms  (librflow_svc_push_frame_t f, uint64_t *out_utc_ms);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_push_frame_get_seq     (librflow_svc_push_frame_t f, uint32_t *out_seq);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_push_frame_get_flush   (librflow_svc_push_frame_t f, bool *out_flush);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_push_frame_get_offset  (librflow_svc_push_frame_t f, uint32_t *out_offset);

/******************************************************************************
 *                          LicenseInfo（只读对象，getter）
 ******************************************************************************/

/*
 * 过期时间（Unix epoch，单位秒，UTC）。
 *   - out_expire_sec 必填；传 NULL 返回 RFLOW_ERR_PARAM；
 *   - License 为永久授权：out_expire_sec = 0，返回 RFLOW_OK；
 *   - License 未成功加载：返回 RFLOW_ERR_NOT_FOUND。
 * 使用 uint64_t 以避免 2038 问题并与平台 time_t 宽度解耦。
 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_license_info_get_expire_time(librflow_svc_license_info_t info,
                                                                           uint64_t *out_expire_sec);

/* 字符串 getter：探测语义与 connect_info_get_* 一致（参见 common.h） */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_license_info_get_vendor_id  (librflow_svc_license_info_t info,
                                                                           char *buf, uint32_t buf_len,
                                                                           uint32_t *out_needed);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_license_info_get_product_key(librflow_svc_license_info_t info,
                                                                           char *buf, uint32_t buf_len,
                                                                           uint32_t *out_needed);
LIBRFLOW_API_EXPORT void        librflow_svc_license_info_destroy        (librflow_svc_license_info_t info);

/******************************************************************************
 *                          生命周期 API
 ******************************************************************************/

/**
 * 设置全局配置。
 *   - 必须在 librflow_svc_init 之前调用；init 之后调用返回 RFLOW_ERR_STATE；
 *   - 可在 init 前多次调用，**后调用者覆盖前者**（last-writer-wins）；
 *   - SDK 在本函数返回前完成深拷贝；调用方可立即 destroy 传入的 cfg；
 *   - svc_uninit 之后可再次调用 svc_set_global_config 并重新 svc_init，实现热重启。
 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_set_global_config(librflow_global_config_t cfg);

/**
 * 初始化（线程池 / SSL / WebRTC 栈 / 编码器后端）。
 *   - **同步**：函数返回即表示 SDK 进入可用态；失败时返回非 OK 且内部状态保持未初始化；
 *   - 幂等：重复调用返回 RFLOW_OK，不重新初始化。
 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_init(void);

/**
 * 反初始化；幂等强清理。
 *   - **同步阻塞**：内部自动 disconnect + 销毁所有 stream，**等待所有工作线程 join**、
 *     所有回调执行完毕后才返回（见 common.h "回调 drain 保证"）；
 *   - 返回后业务可安全释放所有 userdata；
 *   - 不得在任何回调内调用。
 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_uninit(void);

/**
 * 注册设备到服务端。
 *   - **异步**：函数返回 RFLOW_OK 仅表示参数校验通过、连接流程已启动，
 *     此时状态为 CONNECTING；真正 CONNECTED 由 on_state 通知；
 *   - 绑定状态（RFLOW_BIND_*）通过 on_bind_state 通知，与连接状态独立；
 *   - 连接失败（鉴权、网络、License、绑定失败等）通过 on_state(FAILED, reason)
 *     或 on_bind_state(FAILED, extra) 通知，不通过本函数返回值传递；
 *   - 同步返回失败（非 OK）仅表示**未能进入 CONNECTING**：如参数非法、
 *     SDK 未 init、已存在活动连接（RFLOW_ERR_STATE）等；
 *   - info / cb 在本函数返回 OK 时已被 SDK 深拷贝，调用方可立即 destroy。
 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_connect(librflow_svc_connect_info_t info,
                                                     librflow_svc_connect_cb_t   cb);

/**
 * 断开连接；幂等强清理。
 *   - **异步**：函数返回 RFLOW_OK 表示断开流程已启动；所有活动 stream 被 SDK
 *     自动标记为 CLOSING，对应 on_stream_state(CLOSED) 会被异步派发；
 *   - 同步返回后 SDK 保证**不再派发新的**属于该会话的回调（drain 语义见 common.h）；
 *   - 若需"返回即完全停止、所有回调已执行完"，请调用 svc_uninit；
 *   - 可重复调用：已断开状态下调用返回 RFLOW_OK（no-op）。
 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_disconnect(void);

/**
 * 获取当前 license 信息。
 *   - **同步**：仅在 svc_init 之后、license 加载成功时返回 RFLOW_OK；
 *   - 返回的 out_info 需调用方调用 librflow_svc_license_info_destroy 释放；
 *   - 未加载 / 未 init：返回 RFLOW_ERR_STATE，out_info 不被写入。
 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_get_license_info(librflow_svc_license_info_t *out_info);

/******************************************************************************
 *                          流管理
 ******************************************************************************/

/**
 * 创建一路发布流（**同步**：函数 RFLOW_OK 返回后流处于 IDLE，编码器/管道已就绪）
 *
 * @param stream_idx  业务协议中的资源 ID（与 Client 的 index 一一对应）
 * @param param       编码参数（必填）。同步返回后 SDK 已完成 param 深拷贝，调用方可立即 destroy。
 * @param cb          流回调（可为 NULL，表示不关心本地观测）；SDK 深拷贝，可立即 destroy
 * @param out_handle  输出流句柄；RFLOW_OK 时即可用于 start_stream / destroy_stream。
 *
 * 失败返回：
 *   - RFLOW_ERR_STATE       当前未 CONNECTED
 *   - RFLOW_ERR_STREAM_ALREADY_OPEN   同 stream_idx 已有未销毁的流
 *   - RFLOW_ERR_BUSY        达到 RFLOW_LIMIT_MAX_STREAMS
 *   - RFLOW_ERR_STREAM_CODEC_UNSUPP   param 指定了编码器/后端不支持的 codec
 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_create_stream(rflow_stream_index_t          stream_idx,
                                                           librflow_svc_stream_param_t   param,
                                                           librflow_svc_stream_cb_t      cb,
                                                           librflow_svc_stream_handle_t *out_handle);

/**
 * 开始发布流（**异步**：函数返回后流进入 OPENING，真正 OPENED 由 on_stream_state 通知）。
 * OPENING 期间 SDK 允许把 push_video_frame 输入暂存到内部预热缓冲，用于隐藏 WebRTC 建链、
 * 编码器预热和首个关键帧等待时延；该缓冲为 SDK 内部实时有界队列，不改变公开状态机。
 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_start_stream(librflow_svc_stream_handle_t handle);

/**
 * 停止发布流（**异步**：函数返回后流进入 CLOSING，正常完成后派发 on_stream_state(IDLE)）
 * 保留编码器状态，可再次 start_stream。
 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stop_stream (librflow_svc_stream_handle_t handle);

/**
 * 销毁发布流；幂等。
 * **异步**：函数返回后流进入 CLOSING，正常路径最终派发 on_stream_state(CLOSED)。
 * destroy_stream 主动销毁路径不产出 FAILED；CLOSED 回调返回后 handle 真正失效。
 * 收到终态之前 handle 仍可传给其它 API（将返回 RFLOW_ERR_STATE）。
 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_destroy_stream(librflow_svc_stream_handle_t handle);

/**
 * 推送一帧视频（**同步**：SDK 内部 copy 数据后立即返回）。
 *   - OPENING：返回 RFLOW_OK，帧进入 SDK 内部预热缓冲；RFLOW_OK 仅表示帧已被 SDK 接收，
 *     不保证该帧未来一定被实际发送（实时策略下可被后续帧淘汰）；
 *   - OPENED：返回 RFLOW_OK，帧进入正常编码/发送路径；
 *   - IDLE / CLOSING / CLOSED / FAILED：返回 RFLOW_ERR_STATE；
 *   - 单帧 size 超过编码器限制时返回 RFLOW_ERR_PARAM；
 *   - 线程安全：可在任意线程并发 push 不同 handle；对同一 handle 的 push 按调用顺序进入编码队列。
 *   - 预热缓冲在 stop_stream / destroy_stream / disconnect / uninit / FAILED 时全部丢弃。
 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_push_video_frame(librflow_svc_stream_handle_t handle,
                                                              librflow_svc_push_frame_t    frame);

/** 运行期调整目标码率（kbps）；仅在 RC_MODE = CBR/VBR 时生效，CQP 下返回 RFLOW_ERR_NOT_SUPPORT */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_set_bitrate(librflow_svc_stream_handle_t handle,
                                                                uint32_t bitrate_kbps);

/**
 * 主动拉取一次流统计快照
 * @param out_stats  成功时返回一个 retained snapshot；调用方使用完成后必须
 *                   调用 librflow_stream_stats_release
 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_stream_get_stats(librflow_svc_stream_handle_t handle,
                                                              librflow_stream_stats_t *out_stats);

/******************************************************************************
 *                          信令消息收发
 ******************************************************************************/

/*
 * v1 能力边界：
 * - 支持 Service -> Client 单向通知：librflow_svc_send_notice
 * - 支持 Client -> Service 请求，Service 以 librflow_svc_service_reply 异步应答
 * - v1 暂不提供 Service 主动 request Client 并等待 response 的公开 API
 */

/**
 * 主动向 Client/云端推送业务通知（fire-and-forget）。
 * payload 由 SDK 在函数返回前完成拷贝，调用方可立即释放 buffer。
 * 未 CONNECTED 时返回 RFLOW_ERR_STATE；len 超过 RFLOW_LIMIT_MAX_PAYLOAD_BYTES
 * 返回 RFLOW_ERR_PARAM。
 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_send_notice(rflow_notice_index_t index,
                                                         const void *payload,
                                                         uint32_t len);

/**
 * 发布 topic 消息（透传通道，适用于高频状态类上报）
 * 与 send_notice 的区别：不经过业务语义封装，内部直接透传。
 * topic 为 NUL 结尾字符串；payload 同步拷贝；长度上限同 RFLOW_LIMIT_MAX_PAYLOAD_BYTES。
 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_post_topic (const char *topic,
                                                         const void *payload,
                                                         uint32_t len);

/**
 * 对 `on_service_req` 收到的请求异步回包（Service → Client）。
 * 与 Client 侧的 `librflow_reply_to_service_req` 不同：本函数仅在 **Service 库**中，
 * 用于应答对端发来的请求。
 *
 * 所有权：payload 由 SDK 在函数返回前完成拷贝，调用方可立即释放 buffer。
 * 幂等：req_id 仅一次有效；二次调用返回 RFLOW_ERR_NOT_FOUND（已超时或已回包）。
 * 线程：可在任意线程调用，包括 on_service_req 回调内。
 */
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_service_reply(uint64_t req_id,
                                                           rflow_err_t status,
                                                           const void *payload,
                                                           uint32_t len);

/******************************************************************************
 *                          运行期日志 setter（init 后生效）
 ******************************************************************************/
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_log_set_level   (rflow_log_level_t level);
LIBRFLOW_API_EXPORT rflow_err_t librflow_svc_log_set_callback(librflow_log_cb_fn cb, void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* __LIBRFLOW_SERVICE_API_H__ */
