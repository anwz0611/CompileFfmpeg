#include <jni.h>
#include <string>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <mutex>
#include <chrono>
#include <thread>
#include <unistd.h>
#include <cerrno>
#include <cstring>

#define LOG_TAG "FFmpegWrapper"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

// 检查FFmpeg是否可用 - 默认启用，除非明确禁用
#ifndef FFMPEG_FOUND
#define FFMPEG_FOUND 1
#endif

#if FFMPEG_FOUND
extern "C" {
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_mediacodec.h>
#include <libavcodec/mediacodec.h>
#include <libavutil/opt.h>
#include <libavutil/dict.h>
#include <libavutil/time.h>
}

// 编译时配置检查
static void logCompileTimeConfig() {
    LOGI("🔧 编译时配置: FFMPEG_FOUND=%d", FFMPEG_FOUND);
    LOGI("🔧 FFmpeg版本: %s", av_version_info());
}
#else
// 编译时配置检查
static void logCompileTimeConfig() {
    LOGI("❌ 编译时配置: FFMPEG_FOUND=0，录制功能不可用");
}
#endif

// ============================================================================
// 全局渲染状态控制 - 解决Surface生命周期同步问题
// ============================================================================
static std::atomic<bool> g_surface_valid(false);           // Surface是否有效
static std::atomic<bool> g_rendering_paused(false);        // 渲染是否暂停
static std::mutex g_surface_sync_mutex;                    // Surface同步锁
static std::chrono::steady_clock::time_point g_last_surface_change; // 上次Surface变化时间

// ============================================================================
// 现代化MP4录制系统 - 高效RTSP转MP4录制
// ============================================================================
#if FFMPEG_FOUND
class ModernRecorder {
private:
    // 核心FFmpeg组件
    AVFormatContext* output_ctx;
    AVStream* video_stream;
    AVStream* audio_stream;
    AVCodecContext* video_encoder_ctx;
    AVCodecContext* audio_encoder_ctx;
    
    // 录制状态管理
    std::string output_path;
    std::atomic<bool> recording_active;
    std::mutex record_mutex;
    
    // 帧计数和时间管理
    int64_t video_frame_count;
    int64_t audio_frame_count;
    int64_t start_time_us;
    AVRational video_time_base;
    AVRational audio_time_base;
    
    // 录制模式配置
    bool use_hardware_encoding;
    bool copy_video_stream;     // 是否直接复制视频流（不重编码）
    bool copy_audio_stream;     // 是否直接复制音频流（不重编码）
    
    // 性能统计
    int64_t total_video_frames;
    int64_t total_audio_frames;
    int64_t bytes_written;
    
public:
    ModernRecorder() : 
        output_ctx(nullptr), video_stream(nullptr), audio_stream(nullptr),
        video_encoder_ctx(nullptr), audio_encoder_ctx(nullptr),
        recording_active(false), video_frame_count(0), audio_frame_count(0),
        start_time_us(AV_NOPTS_VALUE), use_hardware_encoding(true),
        copy_video_stream(true), copy_audio_stream(true),
        total_video_frames(0), total_audio_frames(0), bytes_written(0) {
        
        video_time_base = {1, 90000};  // 默认90kHz时间基准
        audio_time_base = {1, 48000};  // 默认48kHz时间基准
    }
    
    ~ModernRecorder() {
        cleanup();
    }
    
    // 准备录制 - 设置输出路径和基本参数
    bool prepare(const char* path) {
        std::lock_guard<std::mutex> lock(record_mutex);
        
        if (recording_active.load()) {
            LOGE("🚫 录制器已激活，无法重新准备");
            return false;
        }
        
        if (!path || strlen(path) == 0) {
            LOGE("🚫 输出路径无效");
            return false;
        }
        
        output_path = std::string(path);
        LOGI("📝 录制器准备就绪，输出路径: %s", output_path.c_str());
        
        // 重置计数器
        video_frame_count = 0;
        audio_frame_count = 0;
        total_video_frames = 0;
        total_audio_frames = 0;
        bytes_written = 0;
        start_time_us = AV_NOPTS_VALUE;
        
        return true;
    }
    
    // 启动录制 - 初始化MP4输出格式
    bool start(int width, int height, AVRational framerate) {
        LOGI("🎬 启动MP4录制: %dx%d@%d/%dfps", width, height, framerate.num, framerate.den);
        std::lock_guard<std::mutex> lock(record_mutex);
        
        if (recording_active.load()) {
            LOGE("🚫 录制已激活");
            return false;
        }
        
        if (output_path.empty()) {
            LOGE("🚫 输出路径未设置");
            return false;
        }
        
        // 初始化MP4输出上下文
        if (!initializeOutputContext()) {
            LOGE("❌ 初始化输出上下文失败");
            return false;
        }
        
        // 创建视频流
        if (!createVideoStream(width, height, framerate)) {
            LOGE("❌ 创建视频流失败");
            cleanup();
            return false;
        }
        
        // 打开输出文件并写入头部
        if (!openOutputFile()) {
            LOGE("❌ 打开输出文件失败");
            cleanup();
            return false;
        }
        
        recording_active.store(true);
        start_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        
        LOGI("✅ MP4录制启动成功: %s", output_path.c_str());
        return true;
    }
    
    // 写入视频帧到MP4文件
    bool writeFrame(AVFrame* frame) {
        if (!recording_active.load() || !frame) {
            return false;
        }
        
        std::lock_guard<std::mutex> lock(record_mutex);
        
        if (!output_ctx || !video_stream) {
            return false;
        }
        
        // 如果使用流复制模式，需要重新编码
        if (copy_video_stream) {
            return writeFrameWithCopy(frame);
        } else {
            return writeFrameWithReencode(frame);
        }
    }
    
    // 写入已编码的数据包（更高效的录制方式）
    bool writePacket(AVPacket* packet) {
        if (!recording_active.load() || !packet) {
            return false;
        }
        
        std::lock_guard<std::mutex> lock(record_mutex);
        
        if (!output_ctx || (!video_stream && !audio_stream)) {
            return false;
        }
        
        // 克隆数据包以避免修改原始数据
        AVPacket* pkt = av_packet_clone(packet);
        if (!pkt) {
            return false;
        }
        
        // 根据流类型设置正确的流索引和时间基准
        if (packet->stream_index == 0 && video_stream) {
            // 视频流
            pkt->stream_index = video_stream->index;
            av_packet_rescale_ts(pkt, video_time_base, video_stream->time_base);
            total_video_frames++;
        } else if (packet->stream_index == 1 && audio_stream) {
            // 音频流
            pkt->stream_index = audio_stream->index;
            av_packet_rescale_ts(pkt, audio_time_base, audio_stream->time_base);
            total_audio_frames++;
        } else {
            av_packet_free(&pkt);
            return false;
        }
        
        // 写入交错数据包
        int ret = av_interleaved_write_frame(output_ctx, pkt);
        av_packet_free(&pkt);
        
        if (ret >= 0) {
            bytes_written += packet->size;
            
            // 每1000帧输出一次统计
            if ((total_video_frames + total_audio_frames) % 1000 == 0) {
                LOGD("📊 录制统计: 视频%ld帧, 音频%ld帧, 总计%.1fMB", 
                     (long)total_video_frames, (long)total_audio_frames, bytes_written / 1024.0 / 1024.0);
            }
            return true;
        } else {
            LOGE("❌ 写入数据包失败: %d", ret);
            return false;
        }
    }
    
    // 停止录制并完成MP4文件
    bool stop() {
        LOGI("🛑 停止MP4录制");
        std::lock_guard<std::mutex> lock(record_mutex);
        
        if (!recording_active.load()) {
            LOGI("ℹ️ 录制器未激活，无需停止");
            return true;
    }
    
        recording_active.store(false);
        
        // 刷新编码器缓冲区
        flushEncoders();
        
        // 写入MP4文件尾部
        if (output_ctx) {
            int ret = av_write_trailer(output_ctx);
            if (ret < 0) {
                LOGE("❌ 写入MP4尾部失败: %d", ret);
            } else {
                LOGI("✅ MP4尾部写入成功");
            }
        }
        
        // 输出最终统计
        int64_t current_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t duration_us = current_time_us - start_time_us;
        double duration_sec = duration_us / 1000000.0;
        double file_size_mb = bytes_written / 1024.0 / 1024.0;
        
        LOGI("📊 录制完成统计:");
        LOGI("   📁 文件: %s", output_path.c_str());
        LOGI("   ⏱️ 时长: %.2f秒", duration_sec);
        LOGI("   🎬 视频帧: %ld帧 (%.1ffps)", (long)total_video_frames, total_video_frames / duration_sec);
        LOGI("   🎵 音频帧: %ld帧", (long)total_audio_frames);
        LOGI("   💾 文件大小: %.2fMB", file_size_mb);
        
        return true;
    }
    
    bool isActive() const {
        return recording_active.load();
    }
    
private:
    // 初始化MP4输出上下文
    bool initializeOutputContext() {
        const AVOutputFormat* fmt = av_guess_format("mp4", output_path.c_str(), nullptr);
                if (!fmt) {
            LOGE("❌ 找不到MP4格式");
                    return false;
                }
        
        int ret = avformat_alloc_output_context2(&output_ctx, fmt, nullptr, output_path.c_str());
        if (ret < 0) {
            LOGE("❌ 创建输出上下文失败: %d", ret);
            return false;
        }
        
        LOGI("✅ MP4输出上下文创建成功");
        return true;
    }
    
    // 调试：列出所有可用的H.264编码器
    void listAvailableH264Encoders() {
        LOGI("🔍 列出所有可用的H.264编码器:");
        const AVCodec* codec = nullptr;
        void* opaque = nullptr;
        int count = 0;
        
        while ((codec = av_codec_iterate(&opaque))) {
            if (codec->type == AVMEDIA_TYPE_VIDEO && 
                codec->id == AV_CODEC_ID_H264 && 
                av_codec_is_encoder(codec)) {
                LOGI("  - %s: %s", codec->name, 
                     codec->long_name ? codec->long_name : "无描述");
                count++;
            }
        }
        
        if (count == 0) {
            LOGE("❌ 没有找到任何H.264编码器!");
        } else {
            LOGI("✅ 找到 %d 个H.264编码器", count);
        }
    }
    
    // MediaCodec兼容性检测和自动配置
    bool autoConfigureMediaCodec(AVCodecContext* ctx, int width, int height) {
        LOGI("🔧 自动配置MediaCodec参数");
        
        // 检查分辨率兼容性
        if (width % 16 != 0 || height % 16 != 0) {
            LOGW("⚠️ 分辨率不是16的倍数(%dx%d)，MediaCodec可能不支持", width, height);
            // 调整到16的倍数
            int aligned_width = (width + 15) & ~15;
            int aligned_height = (height + 15) & ~15;
            if (aligned_width != width || aligned_height != height) {
                LOGI("🔧 调整分辨率: %dx%d -> %dx%d", width, height, aligned_width, aligned_height);
                ctx->width = aligned_width;
                ctx->height = aligned_height;
            }
        }
        
        // 检查码率设置
        int64_t recommended_bitrate = width * height * 2; // 2 bits per pixel
        if (ctx->bit_rate != recommended_bitrate) {
            ctx->bit_rate = recommended_bitrate;
            LOGI("🔧 调整码率: %ld bps", (long)ctx->bit_rate);
        }
        
        // 设置兼容性最高的参数组合
        av_opt_set(ctx->priv_data, "profile", "baseline", 0);
        av_opt_set(ctx->priv_data, "level", "3.1", 0);
        av_opt_set(ctx->priv_data, "bitrate_mode", "vbr", 0);
        av_opt_set(ctx->priv_data, "color_format", "nv12", 0);
        av_opt_set_int(ctx->priv_data, "quality", 70, 0);
        av_opt_set_int(ctx->priv_data, "b_frames", 0, 0);
        av_opt_set_int(ctx->priv_data, "g", 30, 0);
        
        // 设置Android优化参数
        av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set_int(ctx->priv_data, "refs", 1, 0);
        
        LOGI("🔧 MediaCodec自动配置完成");
        return true;
    }
    
    // 创建视频流
    bool createVideoStream(int width, int height, AVRational framerate) {
        // 调试：列出可用编码器
        listAvailableH264Encoders();
        // 选择编码器 - 优先硬件编码，但添加兼容性检查
        const AVCodec* codec = nullptr;
        if (use_hardware_encoding) {
            codec = avcodec_find_encoder_by_name("h264_mediacodec");
            if (codec) {
                LOGI("✅ 找到硬件H.264编码器，将测试兼容性");
                
                // 检查分辨率是否被支持（一些设备对分辨率有限制）
                if (width % 16 != 0 || height % 16 != 0) {
                    LOGW("⚠️ 分辨率不是16的倍数(%dx%d)，可能影响硬件编码", width, height);
                }
                
                // 检查是否是常见的分辨率
                bool common_resolution = false;
                if ((width == 1920 && height == 1080) ||
                    (width == 1280 && height == 720) ||
                    (width == 854 && height == 480) ||
                    (width == 640 && height == 480)) {
                    common_resolution = true;
                }
                
                if (!common_resolution) {
                    LOGW("⚠️ 非标准分辨率(%dx%d)，硬件编码可能不稳定", width, height);
                }
            } else {
                LOGW("⚠️ 硬件编码器不可用，使用软件编码器");
                use_hardware_encoding = false;
            }
        }
        
        if (!codec) {
            // 使用与回退相同的智能编码器选择逻辑
            LOGI("🔍 搜索兼容的软件编码器...");
            const char* blacklisted_encoders[] = {
                "h264_mediacodec", "h264_v4l2m2m", "h264_vaapi", 
                "h264_nvenc", "h264_videotoolbox", nullptr
            };
            
            const AVCodec* temp_codec = nullptr;
            void* opaque = nullptr;
            int candidate_count = 0;
            
            while ((temp_codec = av_codec_iterate(&opaque))) {
                if (temp_codec->type == AVMEDIA_TYPE_VIDEO && 
                    temp_codec->id == AV_CODEC_ID_H264 && 
                    av_codec_is_encoder(temp_codec)) {
                    
                    // 检查黑名单
                    bool blacklisted = false;
                    for (int i = 0; blacklisted_encoders[i] != nullptr; i++) {
                        if (strcmp(temp_codec->name, blacklisted_encoders[i]) == 0) {
                            blacklisted = true;
                            break;
                        }
                    }
                    
                    if (!blacklisted) {
                        codec = temp_codec;
                        candidate_count++;
                        LOGI("✅ 候选编码器 #%d: %s", candidate_count, codec->name);
                        
                        // 优先选择纯软件编码器
                        if (strstr(codec->name, "libx264") != nullptr ||
                            strcmp(codec->name, "h264") == 0) {
                            LOGI("🎯 选择优先编码器: %s", codec->name);
                            break;
                        }
                    }
                }
            }
            
            // 如果没有H.264编码器，尝试MJPEG
            if (!codec) {
                LOGW("⚠️ 没有可用的H.264编码器，尝试MJPEG");
                codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
                if (codec) {
                    LOGI("✅ 使用MJPEG编码器: %s", codec->name);
                }
            }
            
                if (!codec) {
                LOGE("❌ 找不到任何兼容的编码器");
                    return false;
                }
            LOGI("✅ 最终选择编码器: %s", codec->name);
            }
        
        // 创建视频流
        video_stream = avformat_new_stream(output_ctx, codec);
        if (!video_stream) {
            LOGE("❌ 创建视频流失败");
            return false;
        }
        
        // 创建编码器上下文
        video_encoder_ctx = avcodec_alloc_context3(codec);
        if (!video_encoder_ctx) {
            LOGE("❌ 分配视频编码器上下文失败");
            return false;
        }
        
        // 设置编码参数
        video_encoder_ctx->width = width;
        video_encoder_ctx->height = height;
        video_encoder_ctx->time_base = av_inv_q(framerate);
        video_encoder_ctx->framerate = framerate;
        video_encoder_ctx->bit_rate = width * height * 2; // 动态码率
        video_encoder_ctx->gop_size = framerate.num; // 1秒一个I帧
        video_encoder_ctx->max_b_frames = 0; // 无B帧，降低延迟
        
        // 编码器优化设置
        if (use_hardware_encoding) {
                    // 使用自动配置系统 - 尝试不同格式解决绿色问题
        video_encoder_ctx->pix_fmt = AV_PIX_FMT_NV12; // Android首选格式
        
        // 自动配置MediaCodec参数
        autoConfigureMediaCodec(video_encoder_ctx, width, height);
        
        // 添加格式回退机制
        LOGI("🎨 设置编码器格式: NV12 (如果出现绿色将自动切换)");
        } else {
            // 软件编码器设置
            video_encoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
            av_opt_set(video_encoder_ctx->priv_data, "preset", "fast", 0);
            av_opt_set(video_encoder_ctx->priv_data, "tune", "zerolatency", 0);
        }
        
        // 全局头部
        if (output_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
            video_encoder_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        
        // 打开编码器
        int ret = avcodec_open2(video_encoder_ctx, codec, nullptr);
        if (ret < 0) {
            char error_buf[256];
            av_strerror(ret, error_buf, sizeof(error_buf));
            LOGE("❌ 打开视频编码器失败: ret=%d, error=%s", ret, error_buf);
            
            // 如果硬件编码器失败，尝试软件编码器
            if (use_hardware_encoding) {
                LOGW("🔄 MediaCodec硬件编码器失败，分析原因并回退");
                
                // 分析具体失败原因
                if (ret == -22) {
                    LOGW("   - 参数无效(EINVAL)：MediaCodec不支持当前参数组合");
                } else if (ret == -542398533) {
                    LOGW("   - 编码器不可用：设备不支持MediaCodec编码器");
                } else if (ret == -61) {
                    LOGW("   - 操作无效：MediaCodec配置冲突");
                }
                
                LOGW("   - 原因分析：可能是颜色格式、码率模式或profile不兼容");
                LOGW("   - 解决方案：回退到软件编码器（libx264）");
                
                avcodec_free_context(&video_encoder_ctx);
                use_hardware_encoding = false;
                
                // 智能编码器选择 - 排除需要特殊权限的编码器
                const char* blacklisted_encoders[] = {
                    "h264_mediacodec",  // MediaCodec（已知失败）
                    "h264_v4l2m2m",     // V4L2（权限问题）
                    "h264_vaapi",       // VA-API（权限问题）
                    "h264_nvenc",       // NVENC（权限问题）
                    "h264_videotoolbox", // VideoToolbox（iOS专用）
                    nullptr
                };
                
                // 搜索所有可用编码器，但排除有问题的
                LOGI("🔍 搜索兼容的软件编码器...");
                const AVCodec* temp_codec = nullptr;
                void* opaque = nullptr;
                int candidate_count = 0;
                
                while ((temp_codec = av_codec_iterate(&opaque))) {
                    if (temp_codec->type == AVMEDIA_TYPE_VIDEO && 
                        temp_codec->id == AV_CODEC_ID_H264 && 
                        av_codec_is_encoder(temp_codec)) {
                        
                        // 检查是否在黑名单中
                        bool blacklisted = false;
                        for (int i = 0; blacklisted_encoders[i] != nullptr; i++) {
                            if (strcmp(temp_codec->name, blacklisted_encoders[i]) == 0) {
                                blacklisted = true;
                                LOGD("🚫 跳过黑名单编码器: %s", temp_codec->name);
                                break;
                            }
                        }
                        
                        if (!blacklisted) {
                            codec = temp_codec;
                            candidate_count++;
                            LOGI("✅ 候选编码器 #%d: %s", candidate_count, codec->name);
                            
                            // 优先选择libx264或纯软件编码器
                            if (strstr(codec->name, "libx264") != nullptr ||
                                strcmp(codec->name, "h264") == 0) {
                                LOGI("🎯 选择优先编码器: %s", codec->name);
                                break;
            }
                        }
                    }
                }
                
                if (candidate_count == 0) {
                    LOGE("❌ 没有找到兼容的H.264编码器");
                }
                
                // 如果H.264编码器都不可用，尝试MJPEG作为备选
                if (!codec) {
                    LOGW("⚠️ 没有可用的H.264编码器，尝试MJPEG备选方案");
                    codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
                    if (codec) {
                        LOGI("✅ 使用MJPEG编码器作为备选: %s", codec->name);
                        // 需要修改输出格式
                        // 但先尝试继续H.264流程
                    } else {
                        LOGE("❌ 连MJPEG编码器也找不到");
                        return false;
                    }
                }
                
                // 重新创建编码器上下文
                video_encoder_ctx = avcodec_alloc_context3(codec);
                if (!video_encoder_ctx) {
                    LOGE("❌ 分配软件编码器上下文失败");
                    return false;
                }
                
                // 重新设置软件编码器参数
                video_encoder_ctx->width = width;
                video_encoder_ctx->height = height;
                video_encoder_ctx->time_base = av_inv_q(framerate);
                video_encoder_ctx->framerate = framerate;
                video_encoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
                video_encoder_ctx->bit_rate = width * height * 2;
                video_encoder_ctx->gop_size = framerate.num;
                video_encoder_ctx->max_b_frames = 0;
                
                // 全局头部
                if (output_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
                    video_encoder_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
                }
                
                // 软件编码器优化
                av_opt_set(video_encoder_ctx->priv_data, "preset", "ultrafast", 0);
                av_opt_set(video_encoder_ctx->priv_data, "tune", "zerolatency", 0);
                av_opt_set(video_encoder_ctx->priv_data, "profile", "baseline", 0);
                
                // 重新尝试打开软件编码器
                ret = avcodec_open2(video_encoder_ctx, codec, nullptr);
        if (ret < 0) {
                    LOGE("❌ 软件编码器也失败: %d", ret);
            return false;
        }
        
                LOGI("✅ 软件编码器初始化成功");
            } else {
                return false;
            }
        }
        
        // 复制参数到流
        ret = avcodec_parameters_from_context(video_stream->codecpar, video_encoder_ctx);
        if (ret < 0) {
            LOGE("❌ 复制视频编码器参数失败: %d", ret);
            return false;
        }
        
        video_stream->time_base = video_encoder_ctx->time_base;
        video_time_base = video_encoder_ctx->time_base;
        
        LOGI("✅ 视频流创建成功: %dx%d@%dfps", width, height, framerate.num);
        return true;
    }
    
    // 打开输出文件并写入头部
    bool openOutputFile() {
        // 打开输出文件
        if (!(output_ctx->oformat->flags & AVFMT_NOFILE)) {
            int ret = avio_open(&output_ctx->pb, output_path.c_str(), AVIO_FLAG_WRITE);
            if (ret < 0) {
                LOGE("❌ 打开输出文件失败: %d", ret);
                return false;
            }
        }
        
        // 写入文件头
        int ret = avformat_write_header(output_ctx, nullptr);
        if (ret < 0) {
            LOGE("❌ 写入MP4头部失败: %d", ret);
            return false;
        }
        
        LOGI("✅ 输出文件打开成功: %s", output_path.c_str());
        return true;
    }
    
    // 刷新编码器缓冲区
    void flushEncoders() {
        // 刷新视频编码器
        if (video_encoder_ctx) {
            avcodec_send_frame(video_encoder_ctx, nullptr);
            
            AVPacket* pkt = av_packet_alloc();
            if (pkt) {
                int ret;
                while ((ret = avcodec_receive_packet(video_encoder_ctx, pkt)) >= 0) {
                    pkt->stream_index = video_stream->index;
                    av_packet_rescale_ts(pkt, video_encoder_ctx->time_base, video_stream->time_base);
                    av_interleaved_write_frame(output_ctx, pkt);
                    av_packet_unref(pkt);
                }
                av_packet_free(&pkt);
            }
        }
        
        // 刷新音频编码器
        if (audio_encoder_ctx) {
            avcodec_send_frame(audio_encoder_ctx, nullptr);
            
            AVPacket* pkt = av_packet_alloc();
            if (pkt) {
                int ret;
                while ((ret = avcodec_receive_packet(audio_encoder_ctx, pkt)) >= 0) {
                    pkt->stream_index = audio_stream->index;
                    av_packet_rescale_ts(pkt, audio_encoder_ctx->time_base, audio_stream->time_base);
                    av_interleaved_write_frame(output_ctx, pkt);
                    av_packet_unref(pkt);
                }
                av_packet_free(&pkt);
    }
        }
    }
    
    // 流复制模式（直接复制已编码的数据）
    bool writeFrameWithCopy(AVFrame* frame) {
        // 对于实时RTSP流，通常使用重编码模式更稳定
        // 流复制需要输入流和输出流的编码参数完全一致
        
        static int copy_mode_log_count = 0;
        if (copy_mode_log_count++ % 100 == 0) {
            LOGD("🔄 使用重编码模式确保兼容性 (第%d次)", copy_mode_log_count);
        }
        
        return writeFrameWithReencode(frame);
    }
    
    // 重新编码模式（将解码帧重新编码为MP4）- 修复绿色问题
    bool writeFrameWithReencode(AVFrame* frame) {
        if (!video_encoder_ctx || !frame) {
            return false;
        }
        
        // 创建编码兼容的帧
        AVFrame* encode_frame = av_frame_alloc();
        if (!encode_frame) {
            return false;
        }
        
        // 设置编码帧参数
        encode_frame->format = video_encoder_ctx->pix_fmt;
        encode_frame->width = video_encoder_ctx->width;
        encode_frame->height = video_encoder_ctx->height;
        encode_frame->pts = video_frame_count++;
        
        // 分配帧缓冲区
        int ret = av_frame_get_buffer(encode_frame, 32);
        if (ret < 0) {
            av_frame_free(&encode_frame);
            return false;
        }
        
        // 修复绿色问题：使用直接数据复制而不是颜色转换
        if (frame->format == 23 && encode_frame->format == AV_PIX_FMT_NV12) {
            // 直接复制数据，避免不必要的颜色转换
            bool copy_success = copyFrameData(frame, encode_frame);
            if (!copy_success) {
                LOGE("❌ 帧数据复制失败");
                av_frame_free(&encode_frame);
                return false;
            }
            LOGD("✅ 使用直接数据复制 (避免绿色问题)");
        } else {
            // 使用颜色空间转换
            bool conversion_success = convertFrameWithSws(frame, encode_frame);
            if (!conversion_success) {
                LOGE("❌ 颜色空间转换失败");
                av_frame_free(&encode_frame);
                return false;
            }
        }
        
        // 发送帧到编码器
        ret = avcodec_send_frame(video_encoder_ctx, encode_frame);
        av_frame_free(&encode_frame);
        
        if (ret < 0) {
            LOGE("❌ 发送帧到编码器失败: %d", ret);
            return false;
        }
        
        // 接收编码后的数据包
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) {
            return false;
        }
        
        bool success = false;
        while ((ret = avcodec_receive_packet(video_encoder_ctx, pkt)) >= 0) {
            pkt->stream_index = video_stream->index;
            av_packet_rescale_ts(pkt, video_encoder_ctx->time_base, video_stream->time_base);
            
            ret = av_interleaved_write_frame(output_ctx, pkt);
            if (ret >= 0) {
                success = true;
                bytes_written += pkt->size;
            }
            av_packet_unref(pkt);
        }
        
        av_packet_free(&pkt);
        return success;
    }
    
    // 专业的颜色空间转换 - 录制专用，修复绿色问题
    bool convertFrameWithSws(AVFrame* src, AVFrame* dst) {
        static SwsContext* record_sws_ctx = nullptr;
        static int cached_src_w = 0, cached_src_h = 0, cached_src_fmt = 0;
        static int cached_dst_w = 0, cached_dst_h = 0, cached_dst_fmt = 0;
        static std::mutex sws_record_mutex;
        
        std::lock_guard<std::mutex> lock(sws_record_mutex);
        
        // 录制专用格式检测 - 修复绿色问题
        AVPixelFormat src_format;
        AVPixelFormat dst_format = (AVPixelFormat)dst->format;
        
        // 对于录制，使用兼容性最好的格式组合
        if (src->format == 23) {
            // MediaCodec格式23，根据数据布局选择合适的源格式
            if (src->linesize[1] == src->linesize[0] && src->data[1] && !src->data[2]) {
                // NV12/NV21格式，使用NV12作为源格式
                src_format = AV_PIX_FMT_NV12;
                LOGD("🎯 录制使用NV12格式 (兼容性最佳)");
            } else if (src->linesize[1] == src->linesize[0]/2 && src->data[1] && src->data[2]) {
                // YUV420P格式
                src_format = AV_PIX_FMT_YUV420P;
                LOGD("🎯 录制使用YUV420P格式");
            } else {
                // 默认使用NV12
                src_format = AV_PIX_FMT_NV12;
                LOGD("🎯 录制默认使用NV12格式");
            }
        } else {
            src_format = (AVPixelFormat)src->format;
        }
        
        LOGD("🎨 录制颜色转换: %dx%d %s -> %dx%d %s", 
             src->width, src->height, av_get_pix_fmt_name(src_format),
             dst->width, dst->height, av_get_pix_fmt_name(dst_format));
        
        // 检查是否需要重建SwsContext - 增加格式兼容性检查
        if (!record_sws_ctx || 
            cached_src_w != src->width || cached_src_h != src->height || cached_src_fmt != src_format ||
            cached_dst_w != dst->width || cached_dst_h != dst->height || cached_dst_fmt != dst_format) {
            
            if (record_sws_ctx) {
                sws_freeContext(record_sws_ctx);
                record_sws_ctx = nullptr;
            }
            
            // 尝试创建SwsContext，如果失败则尝试其他格式
            record_sws_ctx = sws_getContext(
                src->width, src->height, src_format,
                dst->width, dst->height, dst_format,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
            
            if (!record_sws_ctx) {
                LOGW("⚠️ 录制SwsContext创建失败: %s -> %s，尝试回退格式", 
                     av_get_pix_fmt_name(src_format), av_get_pix_fmt_name(dst_format));
                
                // 回退策略：尝试YUV420P源格式
                AVPixelFormat fallback_format = AV_PIX_FMT_YUV420P;
                record_sws_ctx = sws_getContext(
                    src->width, src->height, fallback_format,
                    dst->width, dst->height, dst_format,
                    SWS_BILINEAR, nullptr, nullptr, nullptr);
                
                if (record_sws_ctx) {
                    src_format = fallback_format;
                    LOGI("✅ 录制SwsContext回退成功: %s -> %s", 
                         av_get_pix_fmt_name(src_format), av_get_pix_fmt_name(dst_format));
                } else {
                    LOGE("❌ 录制SwsContext回退也失败");
            return false;
                }
            } else {
                LOGI("✅ 录制SwsContext创建成功: %s -> %s", 
                     av_get_pix_fmt_name(src_format), av_get_pix_fmt_name(dst_format));
            }
            
            cached_src_w = src->width;
            cached_src_h = src->height;
            cached_src_fmt = src_format;
            cached_dst_w = dst->width;
            cached_dst_h = dst->height;
            cached_dst_fmt = dst_format;
        }
        
        // 执行颜色空间转换 - 增加数据验证
        // 验证源数据指针
        if (!src->data[0]) {
            LOGE("❌ 录制源数据指针无效: data[0]=%p", src->data[0]);
            return false;
        }
        
        // 验证目标数据指针
        if (!dst->data[0]) {
            LOGE("❌ 录制目标数据指针无效: data[0]=%p", dst->data[0]);
            return false;
        }
        
        // 验证数据尺寸
        if (src->linesize[0] <= 0 || dst->linesize[0] <= 0) {
            LOGE("❌ 录制数据步长无效: src=%d, dst=%d", src->linesize[0], dst->linesize[0]);
            return false;
        }
        
        int ret = sws_scale(record_sws_ctx, src->data, src->linesize, 0, src->height,
                           dst->data, dst->linesize);
        
        if (ret > 0) {
            LOGD("✅ 录制颜色转换成功: %d行", ret);
            return true;
        } else {
            LOGE("❌ 录制sws_scale失败: ret=%d", ret);
            LOGE("   源格式: %s, 尺寸: %dx%d, linesize: [%d,%d,%d]", 
                 av_get_pix_fmt_name((AVPixelFormat)cached_src_fmt),
                 src->width, src->height, src->linesize[0], src->linesize[1], src->linesize[2]);
            LOGE("   目标格式: %s, 尺寸: %dx%d, linesize: [%d,%d,%d]", 
                 av_get_pix_fmt_name((AVPixelFormat)cached_dst_fmt),
                 dst->width, dst->height, dst->linesize[0], dst->linesize[1], dst->linesize[2]);
            return false;
        }
    }
    
    // 智能检测输入格式 - 增强UV顺序检测
    AVPixelFormat detectInputFormat(AVFrame* frame) {
        if (frame->format != 23) {
            return (AVPixelFormat)frame->format;
        }
        
        // MediaCodec格式23的智能检测
        if (frame->linesize[1] == frame->linesize[0] && frame->data[1] && !frame->data[2]) {
            // 检测UV数据的特征来判断是NV12还是NV21
            return detectNV12vsNV21(frame);
        } else if (frame->linesize[1] == frame->linesize[0]/2 && frame->data[1] && frame->data[2]) {
            return AV_PIX_FMT_YUV420P;
        } else if (frame->data[1] && !frame->data[2]) {
            return detectNV12vsNV21(frame);
        } else {
            return AV_PIX_FMT_YUV420P; // 回退
        }
    }
    
    // 检测NV12 vs NV21格式
    AVPixelFormat detectNV12vsNV21(AVFrame* frame) {
        if (!frame->data[1] || frame->width < 16 || frame->height < 16) {
            return AV_PIX_FMT_NV12; // 默认
        }
        
        // 分析UV数据的统计特征
        uint8_t* uv_data = frame->data[1];
        int uv_stride = frame->linesize[1];
        
        // 采样几个点来检测UV分布
        int u_sum = 0, v_sum = 0;
        int sample_count = 0;
        
        for (int y = 0; y < frame->height/4 && y < 8; y++) {
            for (int x = 0; x < frame->width/4 && x < 16; x += 2) {
                int offset = y * uv_stride + x * 2;
                if (offset + 1 < uv_stride * frame->height/2) {
                    u_sum += uv_data[offset];
                    v_sum += uv_data[offset + 1];
                    sample_count++;
                }
            }
        }
        
        if (sample_count > 0) {
            int u_avg = u_sum / sample_count;
            int v_avg = v_sum / sample_count;
            
            LOGD("🔍 UV检测: U_avg=%d, V_avg=%d, samples=%d", u_avg, v_avg, sample_count);
            
            // 如果V分量明显偏绿色（>140），可能是NV21格式
            if (v_avg > 140 && u_avg < 120) {
                LOGI("🎯 检测到NV21格式 (V偏高: %d)", v_avg);
                return AV_PIX_FMT_NV21;
            }
        }
        
        // 默认使用NV12
        LOGI("🎯 使用NV12格式 (默认)");
        return AV_PIX_FMT_NV12;
    }
    
    // 智能帧格式转换辅助函数 - 支持多种颜色格式
    void convertFrame(AVFrame* src, AVFrame* dst) {
        if (!src || !dst || !src->data[0]) {
            LOGE("❌ convertFrame: 无效的输入参数");
            return;
        }
        
        int src_width = src->width;
        int src_height = src->height;
        int dst_width = dst->width;
        int dst_height = dst->height;
        
        LOGD("🔄 颜色转换: %dx%d (格式%d) -> %dx%d (格式%d)", 
             src_width, src_height, src->format, dst_width, dst_height, dst->format);
        
        // 检测目标格式并进行相应转换
        if (dst->format == AV_PIX_FMT_NV12) {
            // 转换到NV12格式
            convertToNV12(src, dst);
        } else if (dst->format == AV_PIX_FMT_YUV420P) {
            // 转换到YUV420P格式
            convertToYUV420P(src, dst);
        } else {
            LOGE("❌ 不支持的目标格式: %d", dst->format);
            // 回退到基本转换
            convertToYUV420P(src, dst);
        }
    }
    
private:
    // 转换到NV12格式 (Y + UV交错)
    void convertToNV12(AVFrame* src, AVFrame* dst) {
        int src_width = src->width;
        int src_height = src->height;
        int dst_width = dst->width;
        int dst_height = dst->height;
        
        // 1. 转换Y分量
        for (int y = 0; y < dst_height; y++) {
            int src_y = y * src_height / dst_height;
            uint8_t* src_row = src->data[0] + src_y * src->linesize[0];
            uint8_t* dst_row = dst->data[0] + y * dst->linesize[0];
            
            for (int x = 0; x < dst_width; x++) {
                int src_x = x * src_width / dst_width;
                dst_row[x] = src_row[src_x];
            }
        }
        
        // 2. 转换UV分量 (NV12格式：UV交错存储在data[1])
        int uv_dst_width = dst_width / 2;
        int uv_dst_height = dst_height / 2;
            
        if (src->data[1] && src->data[2]) {
            // 源是YUV420P格式，需要将U和V交错存储
            for (int y = 0; y < uv_dst_height; y++) {
                int src_y = y * (src_height / 2) / uv_dst_height;
                
                uint8_t* src_u_row = src->data[1] + src_y * src->linesize[1];
                uint8_t* src_v_row = src->data[2] + src_y * src->linesize[2];
                uint8_t* dst_uv_row = dst->data[1] + y * dst->linesize[1];
                
                for (int x = 0; x < uv_dst_width; x++) {
                    int src_x = x * (src_width / 2) / uv_dst_width;
                    dst_uv_row[x * 2] = src_u_row[src_x];     // U分量
                    dst_uv_row[x * 2 + 1] = src_v_row[src_x]; // V分量
                }
            }
        } else if (src->data[1]) {
            // 源可能已经是NV12/NV21格式，直接复制UV数据
            for (int y = 0; y < uv_dst_height; y++) {
                int src_y = y * (src_height / 2) / uv_dst_height;
                uint8_t* src_uv_row = src->data[1] + src_y * src->linesize[1];
                uint8_t* dst_uv_row = dst->data[1] + y * dst->linesize[1];
                
                for (int x = 0; x < uv_dst_width; x++) {
                    int src_x = x * (src_width / 2) / uv_dst_width;
                    dst_uv_row[x * 2] = src_uv_row[src_x * 2];
                    dst_uv_row[x * 2 + 1] = src_uv_row[src_x * 2 + 1];
    }
            }
        } else {
            // 生成默认的色度值 (128 = 中性灰色)
            LOGW("⚠️ 源帧缺少色度信息，使用默认值");
            for (int y = 0; y < uv_dst_height; y++) {
                uint8_t* dst_uv_row = dst->data[1] + y * dst->linesize[1];
                for (int x = 0; x < uv_dst_width; x++) {
                    dst_uv_row[x * 2] = 128;     // U = 128
                    dst_uv_row[x * 2 + 1] = 128; // V = 128
                }
            }
        }
        
        LOGD("✅ NV12转换完成: Y=%d bytes, UV=%d bytes", 
             dst->linesize[0] * dst_height, dst->linesize[1] * uv_dst_height);
    }
    
        // 转换到YUV420P格式 (Y + U + V分离)
    void convertToYUV420P(AVFrame* src, AVFrame* dst) {
        int src_width = src->width;
        int src_height = src->height;
        int dst_width = dst->width;
        int dst_height = dst->height;
        
        // 1. 转换Y分量
        for (int y = 0; y < dst_height; y++) {
            int src_y = y * src_height / dst_height;
            uint8_t* src_row = src->data[0] + src_y * src->linesize[0];
            uint8_t* dst_row = dst->data[0] + y * dst->linesize[0];
            
            for (int x = 0; x < dst_width; x++) {
                int src_x = x * src_width / dst_width;
                dst_row[x] = src_row[src_x];
            }
        }
        
        // 2. 转换UV分量
        int uv_dst_width = dst_width / 2;
        int uv_dst_height = dst_height / 2;
        
        if (src->data[1] && src->data[2]) {
            // 源是YUV420P格式，直接复制
            for (int y = 0; y < uv_dst_height; y++) {
                int src_y = y * (src_height / 2) / uv_dst_height;
                
                uint8_t* src_u_row = src->data[1] + src_y * src->linesize[1];
                uint8_t* src_v_row = src->data[2] + src_y * src->linesize[2];
                uint8_t* dst_u_row = dst->data[1] + y * dst->linesize[1];
                uint8_t* dst_v_row = dst->data[2] + y * dst->linesize[2];
                
                for (int x = 0; x < uv_dst_width; x++) {
                    int src_x = x * (src_width / 2) / uv_dst_width;
                    dst_u_row[x] = src_u_row[src_x];
                    dst_v_row[x] = src_v_row[src_x];
                }
            }
        } else if (src->data[1]) {
            // 源是NV12/NV21格式，需要分离UV
            for (int y = 0; y < uv_dst_height; y++) {
                int src_y = y * (src_height / 2) / uv_dst_height;
                uint8_t* src_uv_row = src->data[1] + src_y * src->linesize[1];
                uint8_t* dst_u_row = dst->data[1] + y * dst->linesize[1];
                uint8_t* dst_v_row = dst->data[2] + y * dst->linesize[2];
                
                for (int x = 0; x < uv_dst_width; x++) {
                    int src_x = x * (src_width / 2) / uv_dst_width;
                    dst_u_row[x] = src_uv_row[src_x * 2];     // U分量
                    dst_v_row[x] = src_uv_row[src_x * 2 + 1]; // V分量
                }
            }
        } else {
            // 生成默认的色度值
            LOGW("⚠️ 源帧缺少色度信息，使用默认值");
            memset(dst->data[1], 128, dst->linesize[1] * uv_dst_height);
            memset(dst->data[2], 128, dst->linesize[2] * uv_dst_height);
        }
        
        LOGD("✅ YUV420P转换完成");
    }
    
    // 直接数据复制 - 修复绿色问题的关键函数
    bool copyFrameData(AVFrame* src, AVFrame* dst) {
        if (!src || !dst || !src->data[0] || !dst->data[0]) {
            LOGE("❌ 无效的帧指针");
            return false;
        }
        
        // 1. 复制Y分量
        int y_height = std::min(src->height, dst->height);
        int y_width = std::min(src->linesize[0], dst->linesize[0]);
        
        for (int y = 0; y < y_height; y++) {
            memcpy(dst->data[0] + y * dst->linesize[0],
                   src->data[0] + y * src->linesize[0],
                   y_width);
        }
        
        // 2. 复制UV分量
        if (src->data[1] && dst->data[1]) {
            int uv_height = std::min(src->height / 2, dst->height / 2);
            int uv_width = std::min(src->linesize[1], dst->linesize[1]);
            
            for (int y = 0; y < uv_height; y++) {
                memcpy(dst->data[1] + y * dst->linesize[1],
                       src->data[1] + y * src->linesize[1],
                       uv_width);
            }
            
            LOGD("✅ 直接数据复制完成: Y=%dx%d, UV=%dx%d", 
                 y_width, y_height, uv_width, uv_height);
        } else {
            LOGW("⚠️ 源或目标缺少UV数据");
            return false;
        }
        
        return true;
    }

public:
    

    
    // 清理所有资源
    void cleanup() {
        std::lock_guard<std::mutex> lock(record_mutex);
        
        LOGI("🧹 清理录制器资源");
        recording_active.store(false);
        
        // 关闭视频编码器
        if (video_encoder_ctx) {
            avcodec_free_context(&video_encoder_ctx);
            video_encoder_ctx = nullptr;
        }
        
        // 关闭音频编码器
        if (audio_encoder_ctx) {
            avcodec_free_context(&audio_encoder_ctx);
            audio_encoder_ctx = nullptr;
        }
        
        // 关闭输出上下文
        if (output_ctx) {
            // 关闭输出文件
            if (!(output_ctx->oformat->flags & AVFMT_NOFILE) && output_ctx->pb) {
                avio_closep(&output_ctx->pb);
            }
            avformat_free_context(output_ctx);
            output_ctx = nullptr;
        }
        
        // 重置流指针
        video_stream = nullptr;
        audio_stream = nullptr;
        
        // 重置统计数据
        video_frame_count = 0;
        audio_frame_count = 0;
        total_video_frames = 0;
        total_audio_frames = 0;
        bytes_written = 0;
        start_time_us = AV_NOPTS_VALUE;
        
        LOGI("✅ 录制器资源清理完成");
    }
};

// 全局录制器实例
static ModernRecorder* g_recorder = nullptr;
static std::mutex g_recorder_mutex;
#endif

// ============================================================================
// 超低延迟播放核心模块 - 独立封装，不允许外部修改
// ============================================================================
#if FFMPEG_FOUND
class UltraLowLatencyPlayer {
private:
    // 核心播放状态
    AVFormatContext* input_ctx;
    AVCodecContext* decoder_ctx;
    AVFrame* decode_frame;
    int video_stream_index;
    
    // 延迟控制参数
    static const int MAX_FRAME_BUFFER = 1;      // 最大帧缓冲：1帧
    static const int EMERGENCY_DROP_THRESHOLD = 2; // 紧急丢帧阈值
    static const int MAX_DECODE_TIME_MS = 33;   // 最大解码时间：33ms(30fps)
    
    // 性能监控
    std::chrono::steady_clock::time_point last_frame_time;
    int consecutive_slow_frames;
    int total_dropped_frames;
    
    // 缓冲区控制
    int pending_frames_count;
    std::chrono::steady_clock::time_point last_drop_time;
    
    // 硬件解码状态
    bool hardware_decode_available;
    
    // 录制专用帧缓存
    AVFrame* record_frame;
    std::mutex record_frame_mutex;
    std::chrono::steady_clock::time_point last_record_frame_time;
    
public:
    UltraLowLatencyPlayer() : 
        input_ctx(nullptr), decoder_ctx(nullptr), 
        decode_frame(nullptr), video_stream_index(-1),
        consecutive_slow_frames(0), total_dropped_frames(0),
        pending_frames_count(0), hardware_decode_available(false),
        record_frame(nullptr) {
        
        last_frame_time = std::chrono::steady_clock::now();
        last_drop_time = std::chrono::steady_clock::now();
        last_record_frame_time = std::chrono::steady_clock::now();
    }
    
    ~UltraLowLatencyPlayer() {
        cleanup();
    }
    
    // 初始化播放器 - 超低延迟配置
    bool initialize(const char* rtsp_url) {
        LOGI("🚀 初始化超低延迟播放器: %s", rtsp_url);
        
        // 创建输入上下文
        input_ctx = avformat_alloc_context();
        if (!input_ctx) {
            LOGE("❌ 分配输入上下文失败");
            return false;
        }
        
        // 激进的超低延迟配置
        AVDictionary *options = nullptr;
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        av_dict_set(&options, "stimeout", "1000000", 0);        // 1秒超时
        av_dict_set(&options, "max_delay", "0", 0);             // 零延迟（激进）
        av_dict_set(&options, "buffer_size", "32768", 0);       // 32KB最小缓冲
        av_dict_set(&options, "fflags", "nobuffer+flush_packets+discardcorrupt", 0);
        av_dict_set(&options, "flags", "low_delay", 0);
        av_dict_set(&options, "probesize", "4096", 0);          // 4KB探测
        av_dict_set(&options, "analyzeduration", "10000", 0);   // 10ms分析
        av_dict_set(&options, "reorder_queue_size", "0", 0);    // 禁用重排序
        
        int ret = avformat_open_input(&input_ctx, rtsp_url, nullptr, &options);
        av_dict_free(&options);
        
        if (ret < 0) {
            LOGE("❌ 打开RTSP流失败: %d", ret);
            cleanup();
            return false;
        }
        
        // 快速获取流信息
        ret = avformat_find_stream_info(input_ctx, nullptr);
        if (ret < 0) {
            LOGE("❌ 获取流信息失败: %d", ret);
            cleanup();
            return false;
        }
        
        // 查找视频流
        video_stream_index = -1;
        for (unsigned int i = 0; i < input_ctx->nb_streams; i++) {
            if (input_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                video_stream_index = i;
                break;
            }
        }
        
        if (video_stream_index == -1) {
            LOGE("❌ 未找到视频流");
            cleanup();
            return false;
        }
        
        // 初始化解码器
        if (!initializeDecoder()) {
            cleanup();
            return false;
        }
        
        // 应用激进的低延迟设置
        input_ctx->flags |= AVFMT_FLAG_NOBUFFER;
        input_ctx->flags |= AVFMT_FLAG_FLUSH_PACKETS;
        input_ctx->max_delay = 0;  // 零延迟
        
        // 分配解码帧
        decode_frame = av_frame_alloc();
        if (!decode_frame) {
            LOGE("❌ 分配解码帧失败");
            cleanup();
            return false;
        }
        
        LOGI("✅ 超低延迟播放器初始化成功");
        return true;
    }
    
    // 处理一帧 - 核心播放逻辑
    bool processFrame() {
        if (!input_ctx || !decoder_ctx || !decode_frame) {
            static int init_check_count = 0;
            if (init_check_count++ % 10 == 0) {
                LOGE("❌ 播放器组件未初始化: input_ctx=%p, decoder_ctx=%p, decode_frame=%p", 
                     input_ctx, decoder_ctx, decode_frame);
            }
            return false;
        }
        
        auto frame_start = std::chrono::steady_clock::now();
        
        // 读取数据包
        AVPacket *pkt = av_packet_alloc();
        if (!pkt) {
            LOGE("❌ 分配数据包失败");
            return false;
        }
        
        int ret = av_read_frame(input_ctx, pkt);
        if (ret < 0) {
            av_packet_free(&pkt);
            if (ret == AVERROR(EAGAIN)) {
                return true; // 暂时没有数据，继续尝试
            }
            
            // 详细的错误分析
            static int read_error_count = 0;
            if (read_error_count++ % 5 == 0) {
                char error_buf[256];
                av_strerror(ret, error_buf, sizeof(error_buf));
                LOGE("❌ 读取帧失败 (第%d次): ret=%d, error=%s", read_error_count, ret, error_buf);
                
                if (ret == AVERROR_EOF) {
                    LOGE("   - 流已结束 (EOF)");
                } else if (ret == AVERROR(ECONNRESET)) {
                    LOGE("   - 网络连接重置");
                } else if (ret == AVERROR(ETIMEDOUT)) {
                    LOGE("   - 网络超时");
                }
            }
            
            return false;
        }
        
        // 记录第一次成功读取数据包
        static bool first_packet_read = false;
        if (!first_packet_read) {
            LOGI("✅ 第一次成功读取数据包: stream_index=%d, size=%d, pts=%ld", 
                 pkt->stream_index, pkt->size, (long)pkt->pts);
            first_packet_read = true;
        }
        
        // 只处理视频帧
        if (pkt->stream_index != video_stream_index) {
            av_packet_free(&pkt);
            return true;
        }
        
        // 临时禁用数据包录制以避免死锁
        // TODO: 需要重新设计录制架构来避免锁冲突
        
        // 发送到解码器
        ret = avcodec_send_packet(decoder_ctx, pkt);
        
        // 记录第一次发送数据包的结果
        static bool first_send_logged = false;
        if (!first_send_logged) {
            if (ret >= 0) {
                LOGI("✅ 第一次发送数据包成功: ret=%d", ret);
            } else {
                char error_buf[256];
                av_strerror(ret, error_buf, sizeof(error_buf));
                LOGE("❌ 第一次发送数据包失败: ret=%d, error=%s", ret, error_buf);
            }
            first_send_logged = true;
        }
        
        av_packet_free(&pkt);
        
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            static int send_error_count = 0;
            if (send_error_count++ % 5 == 0) {
                char error_buf[256];
                av_strerror(ret, error_buf, sizeof(error_buf));
                LOGE("❌ 发送数据包失败 (第%d次): ret=%d, error=%s", send_error_count, ret, error_buf);
            }
            return false;
        }
        
        // 接收解码帧 - 智能帧管理
        bool frame_received = false;
        int frames_received_this_call = 0;
        bool has_valid_frame = false;
        
        // 接收第一个可用帧
        ret = avcodec_receive_frame(decoder_ctx, decode_frame);
        
        // 记录第一次接收帧的尝试
        static bool first_receive_logged = false;
        if (!first_receive_logged) {
            if (ret == AVERROR(EAGAIN)) {
                LOGI("ℹ️ 第一次接收帧: 需要更多数据包 (EAGAIN)");
            } else if (ret >= 0) {
                LOGI("✅ 第一次接收帧成功: ret=%d", ret);
            } else {
                char error_buf[256];
                av_strerror(ret, error_buf, sizeof(error_buf));
                LOGE("❌ 第一次接收帧失败: ret=%d, error=%s", ret, error_buf);
            }
            first_receive_logged = true;
        }
        
        if (ret == AVERROR(EAGAIN)) {
            // 没有帧可接收，这是正常的
        } else if (ret < 0) {
            static int receive_error_count = 0;
            if (receive_error_count++ % 5 == 0) {
                char error_buf[256];
                av_strerror(ret, error_buf, sizeof(error_buf));
                LOGE("❌ 接收帧失败 (第%d次): ret=%d, error=%s", receive_error_count, ret, error_buf);
            }
            return false;
        } else {
            // 成功接收到第一帧
            frames_received_this_call++;
            frame_received = true;
            
            // 检查帧是否有效
            if (decode_frame->width > 0 && decode_frame->height > 0 && 
                (decode_frame->data[0] || decode_frame->data[1] || decode_frame->data[3])) {
                has_valid_frame = true;
                
                // 记录第一次成功接收帧
                static bool first_frame_received = false;
                if (!first_frame_received) {
                    LOGI("✅ 第一次成功接收解码帧: %dx%d, format=%d, data[0]=%p", 
                         decode_frame->width, decode_frame->height, decode_frame->format, decode_frame->data[0]);
                    first_frame_received = true;
                }
            }
            
            // 继续接收剩余帧以清空缓冲区，但不覆盖有效帧
            AVFrame* temp_frame = av_frame_alloc();
            if (temp_frame) {
                while (true) {
                    ret = avcodec_receive_frame(decoder_ctx, temp_frame);
                    if (ret == AVERROR(EAGAIN) || ret < 0) {
                        break; // 没有更多帧或出错
                    }
                    
                    frames_received_this_call++;
                    total_dropped_frames++;
                    
                            // 每丢弃50帧输出一次日志（减少日志频率）
        if (total_dropped_frames % 50 == 0) {
            LOGD("🗑️ 丢弃旧帧以保持超低延迟 (累计丢弃: %d)", total_dropped_frames);
        }
                }
                av_frame_free(&temp_frame);
            }
        }
        
        // 性能统计（减少日志输出）
        if (frame_received) {
            static int frame_count = 0;
            frame_count++;
            // 只在关键节点输出日志
            if (frame_count <= 3 || frame_count % 100 == 0) {
                LOGD("🎯 processFrame #%d: 接收%d帧, 有效帧=%s, 尺寸=%dx%d", 
                     frame_count, frames_received_this_call, has_valid_frame ? "是" : "否",
                     decode_frame->width, decode_frame->height);
            }
            
            // 更新录制帧 - 仅在有有效帧时更新
            if (has_valid_frame) {
                updateRecordFrame();
            }
        }
        
        // 检查解码性能
        auto frame_end = std::chrono::steady_clock::now();
        auto decode_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            frame_end - frame_start).count();
        
        if (decode_time > MAX_DECODE_TIME_MS) {
            consecutive_slow_frames++;
            if (consecutive_slow_frames > 3) {
                LOGW("⚠️ 连续慢解码，考虑降低质量或跳帧");
            }
        } else {
            consecutive_slow_frames = 0;
        }
        
        // 更新时间戳
        last_frame_time = frame_end;
        
        // 性能监控和统计
        static bool first_process_result_logged = false;
        static int total_processed_frames = 0;
        
        if (!first_process_result_logged) {
            LOGI("📊 第一次processFrame完成: frame_received=%s, frames_received=%d, decode_time=%lldms", 
                 frame_received ? "true" : "false", frames_received_this_call, decode_time);
            first_process_result_logged = true;
        }
        
        // 每处理100帧输出一次性能统计
        if (frame_received) {
            total_processed_frames++;
            if (total_processed_frames % 100 == 0) {
                float drop_rate = (float)total_dropped_frames / total_processed_frames * 100;
                LOGI("📊 播放统计: 已处理%d帧, 丢弃%d帧(%.1f%%), 慢解码%d次", 
                     total_processed_frames, total_dropped_frames, drop_rate, consecutive_slow_frames);
            }
        }
        
        // 关键修复：即使没有接收到帧，只要成功读取了数据包就返回true
        // 这对于需要多个数据包才能产生帧的编码格式（如HEVC）是正常的
        return true;
    }
    
    // 获取当前解码帧 - 只有在真正有有效帧时才返回
    AVFrame* getCurrentFrame() {
        // 性能优化：减少调试日志
        static int call_count = 0;
        call_count++;
        
        // 检查decode_frame是否存在
        if (!decode_frame) {
            return nullptr;
        }
        
        // 检查基本尺寸
        if (decode_frame->width <= 0 || decode_frame->height <= 0) {
            return nullptr;
        }
        
        // 对于MediaCodec硬件解码（format=23）
        if (decode_frame->format == 23) {
            bool has_data = decode_frame->data[0] || decode_frame->data[1] || decode_frame->data[3];
            
            // 只在关键时刻输出日志
            static bool first_mediacodec_logged = false;
            if (!first_mediacodec_logged && has_data) {
                LOGI("🔍 MediaCodec帧验证成功: %dx%d, format=%d", 
                     decode_frame->width, decode_frame->height, decode_frame->format);
                first_mediacodec_logged = true;
            }
            
            return has_data ? decode_frame : nullptr;
        } else {
            // 普通格式，必须有data[0]
            bool has_data = decode_frame->data[0] != nullptr;
            
            // 只在第一次成功时输出日志
            static bool first_software_logged = false;
            if (!first_software_logged && has_data) {
                LOGI("🔍 软件解码帧验证成功: %dx%d, format=%d", 
                     decode_frame->width, decode_frame->height, decode_frame->format);
                first_software_logged = true;
            }
            
            return has_data ? decode_frame : nullptr;
        }
    }
    
    // 获取录制专用帧 - 确保录制时有稳定的帧数据
    AVFrame* getRecordFrame() {
        std::lock_guard<std::mutex> lock(record_frame_mutex);
        
        if (record_frame) {
            auto now = std::chrono::steady_clock::now();
            auto frame_age = std::chrono::duration_cast<std::chrono::seconds>(
                now - last_record_frame_time).count();
            
            if (frame_age > 3) {
                av_frame_unref(record_frame);
                av_frame_free(&record_frame);
                record_frame = nullptr;
            }
        }
        
        return record_frame;
    }
    
    void updateRecordFrame() {
        std::lock_guard<std::mutex> lock(record_frame_mutex);
        
        AVFrame* current = getCurrentFrame();
        if (!current) {
            return;
        }
        
        if (!record_frame) {
            record_frame = av_frame_alloc();
            if (!record_frame) {
                return;
            }
        } else {
            av_frame_unref(record_frame);
        }
        
        if (av_frame_ref(record_frame, current) >= 0) {
            last_record_frame_time = std::chrono::steady_clock::now();
        } else {
            av_frame_free(&record_frame);
            record_frame = nullptr;
        }
    }
    
    // 是否使用硬件解码
    bool isHardwareDecoding() const {
        return hardware_decode_available;
    }
    
    // 获取性能统计
    void getStats(int& dropped_frames, int& slow_frames) {
        dropped_frames = total_dropped_frames;
        slow_frames = consecutive_slow_frames;
    }
    
    void flushBuffers() {
        if (decoder_ctx) {
            avcodec_flush_buffers(decoder_ctx);
        }
        pending_frames_count = 0;
        consecutive_slow_frames = 0;
    }
    
    // 清理资源
    void cleanup() {
        // 清理录制帧
        {
            std::lock_guard<std::mutex> lock(record_frame_mutex);
            if (record_frame) {
                av_frame_free(&record_frame);
                record_frame = nullptr;
            }
        }
        
        if (decode_frame) {
            av_frame_free(&decode_frame);
            decode_frame = nullptr;
        }
        
        if (decoder_ctx) {
            avcodec_free_context(&decoder_ctx);
            decoder_ctx = nullptr;
        }
        
        if (input_ctx) {
            avformat_close_input(&input_ctx);
            input_ctx = nullptr;
        }
        
        video_stream_index = -1;
        hardware_decode_available = false;
    }
    
private:
    // 初始化解码器
    bool initializeDecoder() {
        AVStream* video_stream = input_ctx->streams[video_stream_index];
        AVCodecID codec_id = video_stream->codecpar->codec_id;
        
        const AVCodec *decoder = nullptr;
        
        // 尝试硬件解码器
        if (codec_id == AV_CODEC_ID_H264) {
            decoder = avcodec_find_decoder_by_name("h264_mediacodec");
            if (decoder) {
                hardware_decode_available = true;
                LOGI("✅ 使用H.264硬件解码器");
            }
        } else if (codec_id == AV_CODEC_ID_HEVC) {
            decoder = avcodec_find_decoder_by_name("hevc_mediacodec");
            if (decoder) {
                hardware_decode_available = true;
                LOGI("✅ 使用HEVC硬件解码器");
            }
        }
        
        // 回退到软件解码器
        if (!decoder) {
            decoder = avcodec_find_decoder(codec_id);
            if (decoder) {
                hardware_decode_available = false;
                LOGI("✅ 使用软件解码器: %s", decoder->name);
            } else {
                LOGE("❌ 未找到解码器");
                return false;
            }
        }
        
        // 分配解码器上下文
        decoder_ctx = avcodec_alloc_context3(decoder);
        if (!decoder_ctx) {
            LOGE("❌ 分配解码器上下文失败");
            return false;
        }
        
        // 复制参数
        int ret = avcodec_parameters_to_context(decoder_ctx, video_stream->codecpar);
        if (ret < 0) {
            LOGE("❌ 复制解码器参数失败: %d", ret);
            return false;
        }
        
        // 超低延迟配置
        decoder_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        decoder_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
        decoder_ctx->thread_count = 1;              // 单线程避免重排序
        decoder_ctx->thread_type = FF_THREAD_SLICE;
        decoder_ctx->delay = 0;                     // 零延迟
        decoder_ctx->has_b_frames = 0;              // 禁用B帧
        decoder_ctx->max_b_frames = 0;
        decoder_ctx->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
        
        // 软件解码器额外优化
        if (!hardware_decode_available) {
            decoder_ctx->skip_frame = AVDISCARD_NONREF;
            decoder_ctx->skip_idct = AVDISCARD_BIDIR;
            decoder_ctx->skip_loop_filter = AVDISCARD_BIDIR;
        }
        
        // 对于硬件解码器，尝试设置Surface输出
        AVDictionary* opts = nullptr;
        if (hardware_decode_available) {
            // 设置MediaCodec为Surface模式（如果有Surface可用）
            // 注意：这里我们先尝试buffer模式，稍后可以添加surface模式支持
            LOGI("🔧 配置MediaCodec硬件解码器");
        }
        
        // 打开解码器
        ret = avcodec_open2(decoder_ctx, decoder, &opts);
        if (opts) {
            av_dict_free(&opts);
        }
        
        if (ret < 0) {
            char error_buf[256];
            av_strerror(ret, error_buf, sizeof(error_buf));
            LOGE("❌ 打开解码器失败: ret=%d, error=%s", ret, error_buf);
            
            // 如果硬件解码器失败，尝试软件解码器
            if (hardware_decode_available) {
                LOGW("🔄 硬件解码器失败，尝试软件解码器");
                avcodec_free_context(&decoder_ctx);
                
                // 获取软件解码器
                decoder = avcodec_find_decoder(codec_id);
                if (!decoder) {
                    LOGE("❌ 未找到软件解码器");
                    return false;
                }
                
                // 重新分配解码器上下文
                decoder_ctx = avcodec_alloc_context3(decoder);
                if (!decoder_ctx) {
                    LOGE("❌ 重新分配解码器上下文失败");
                    return false;
                }
                
                // 重新复制参数
                ret = avcodec_parameters_to_context(decoder_ctx, video_stream->codecpar);
                if (ret < 0) {
                    LOGE("❌ 重新复制解码器参数失败: %d", ret);
                    return false;
                }
                
                // 软件解码器配置
                decoder_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
                decoder_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
                decoder_ctx->thread_count = 1;
                decoder_ctx->thread_type = FF_THREAD_SLICE;
                decoder_ctx->delay = 0;
                decoder_ctx->has_b_frames = 0;
                decoder_ctx->max_b_frames = 0;
                decoder_ctx->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
                decoder_ctx->skip_frame = AVDISCARD_NONREF;
                decoder_ctx->skip_idct = AVDISCARD_BIDIR;
                decoder_ctx->skip_loop_filter = AVDISCARD_BIDIR;
                
                // 尝试打开软件解码器
                ret = avcodec_open2(decoder_ctx, decoder, nullptr);
                if (ret < 0) {
                    av_strerror(ret, error_buf, sizeof(error_buf));
                    LOGE("❌ 软件解码器也失败: ret=%d, error=%s", ret, error_buf);
                    return false;
                }
                
                hardware_decode_available = false;
                LOGI("✅ 使用软件解码器: %s", decoder->name);
            } else {
                return false;
            }
        }
        
        LOGI("✅ 解码器初始化成功 (硬件解码: %s)", hardware_decode_available ? "是" : "否");
        return true;
    }
};

// 全局播放器实例
static UltraLowLatencyPlayer* g_player = nullptr;
static std::mutex g_player_mutex;
#endif

// ============================================================================
// 渲染核心模块 - 独立封装
// ============================================================================
class UltraLowLatencyRenderer {
private:
    ANativeWindow* native_window;
    SwsContext* sws_ctx;
    std::mutex render_mutex;
    
    // 渲染控制
    std::chrono::steady_clock::time_point last_render_time;
    static const int MIN_RENDER_INTERVAL_MS = 16; // 最大60fps
    
    // 缓存的SwsContext参数
    int cached_src_width, cached_src_height;
    AVPixelFormat cached_src_format;
    int cached_dst_width, cached_dst_height;
    
public:
    UltraLowLatencyRenderer() : 
        native_window(nullptr), sws_ctx(nullptr),
        cached_src_width(0), cached_src_height(0), 
        cached_src_format(AV_PIX_FMT_NONE),
        cached_dst_width(0), cached_dst_height(0) {
        
        last_render_time = std::chrono::steady_clock::now();
    }
    
    ~UltraLowLatencyRenderer() {
        cleanup();
    }
    
    // 设置渲染目标 - 增强稳定性版本
    bool setSurface(ANativeWindow* window) {
        std::lock_guard<std::mutex> lock(render_mutex);
        std::lock_guard<std::mutex> sync_lock(g_surface_sync_mutex);
        
        // 暂停渲染，确保线程安全
        g_rendering_paused = true;
        g_surface_valid = false;
        
        // 等待当前渲染完成（最多等待33ms，确保超低延迟）
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        
        // 安全释放旧资源
        if (native_window) {
            ANativeWindow_release(native_window);
            native_window = nullptr;
        }
        
        // 强制重建SwsContext，避免状态不一致
        if (sws_ctx) {
            sws_freeContext(sws_ctx);
            sws_ctx = nullptr;
            cached_src_width = 0;
            cached_src_height = 0;
            cached_src_format = AV_PIX_FMT_NONE;
        }
        
        // 设置新Surface
        native_window = window;
        g_last_surface_change = std::chrono::steady_clock::now();
        
        if (native_window) {
            g_surface_valid = true;
            g_rendering_paused = false;
            LOGI("✅ 渲染器Surface设置成功，恢复渲染");
            return true;
        } else {
            LOGI("🧹 渲染器Surface已清理，保持暂停状态");
            return true;
        }
    }
    
    // 渲染帧 - 核心渲染逻辑（增强稳定性）
    bool renderFrame(AVFrame* frame) {
        // 第一层检查：基本参数有效性
        if (!frame || !native_window) {
            return false;
        }
        
        // 第二层检查：Surface状态同步
        if (!g_surface_valid || g_rendering_paused) {
            return false; // 快速返回，保持超低延迟
        }
        
        std::lock_guard<std::mutex> lock(render_mutex);
        
        // 第三层检查：再次验证资源有效性（防止竞态条件）
        if (!native_window || !g_surface_valid) {
            return false;
        }
        
        // 帧率控制
        auto now = std::chrono::steady_clock::now();
        auto time_since_last = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_render_time).count();
        
        if (time_since_last < MIN_RENDER_INTERVAL_MS) {
            return true; // 跳过渲染，保持帧率稳定
        }
        
        // 记录第一次渲染尝试
        static bool first_render_logged = false;
        if (!first_render_logged) {
            LOGI("🎬 第一次渲染尝试: format=%d, data[0]=%p, data[3]=%p, width=%d, height=%d", 
                 frame->format, frame->data[0], frame->data[3], frame->width, frame->height);
            first_render_logged = true;
        }
        
        // 检查是否是硬件解码帧 - MediaCodec buffer模式
        if (frame->format == 23) {
            // MediaCodec硬件解码输出，需要转换为可渲染格式
            // 减少日志输出以提升性能
            
            // 对于MediaCodec buffer模式，我们需要软件渲染路径
            // 但首先要正确处理MediaCodec buffer
            if (frame->data[3] != nullptr) {
                // 释放MediaCodec buffer（如果需要）
                // 注意：不要立即释放，因为我们还需要数据
                LOGD("📦 MediaCodec buffer模式，准备软件渲染");
            }
        }
        
        // 软件渲染路径 - 处理所有格式（包括MediaCodec buffer）
        return renderFrameSoftware(frame);
    }
    
    void cleanup() {
        std::lock_guard<std::mutex> lock(render_mutex);
        
        if (sws_ctx) {
            sws_freeContext(sws_ctx);
            sws_ctx = nullptr;
        }
        
        if (native_window) {
            ANativeWindow_release(native_window);
            native_window = nullptr;
        }
        
        cached_src_width = 0;
    }
    
private:
    // 软件渲染实现（增强稳定性）
    bool renderFrameSoftware(AVFrame* frame) {
        // 关键安全检查：确保渲染资源有效
        if (!native_window || !g_surface_valid || g_rendering_paused) {
            LOGW("⚠️ 渲染资源无效，跳过此帧: native_window=%p, valid=%d, paused=%d", 
                 native_window, (int)g_surface_valid, (int)g_rendering_paused);
            return false;
        }
        
        // 检查Surface变化时间，避免过于频繁的重建
        auto now = std::chrono::steady_clock::now();
        auto surface_age = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - g_last_surface_change).count();
        if (surface_age < 50) { // Surface创建后50ms内暂缓渲染，确保稳定
            return false;
        }
        
        // 设置缓冲区格式（每次Surface变化后重新设置）
        static bool format_set = false;
        if (!format_set || surface_age < 100) {
            int ret = ANativeWindow_setBuffersGeometry(native_window, 
                frame->width, frame->height, WINDOW_FORMAT_RGBA_8888);
            if (ret != 0) {
                LOGE("❌ 设置Surface缓冲区失败: %d", ret);
                return false;
            }
            format_set = true;
        }
        
        // 检测输入格式
        AVPixelFormat input_format = detectPixelFormat(frame);
        
        // 更新SwsContext
        if (!updateSwsContext(frame, input_format)) {
            return false;
        }
        
        // 锁定Surface前再次检查有效性
        if (!g_surface_valid || !native_window) {
            LOGW("⚠️ Surface在锁定前变为无效");
            return false;
        }
        
        ANativeWindow_Buffer buffer;
        int ret = ANativeWindow_lock(native_window, &buffer, nullptr);
        if (ret != 0) {
            LOGE("❌ 锁定Surface失败: %d", ret);
            return false;
        }
        
        // 执行颜色空间转换前的最后检查
        if (!sws_ctx || !g_surface_valid) {
            ANativeWindow_unlockAndPost(native_window); // 确保解锁
            LOGW("⚠️ SwsContext或Surface在转换前失效");
            return false;
        }
        
        uint8_t* dst_data[4] = {(uint8_t*)buffer.bits, nullptr, nullptr, nullptr};
        int dst_linesize[4] = {buffer.stride * 4, 0, 0, 0};
        
        ret = sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height,
                       dst_data, dst_linesize);
        
        // 解锁并显示
        ANativeWindow_unlockAndPost(native_window);
        
        if (ret > 0) {
            last_render_time = std::chrono::steady_clock::now();
            return true;
        } else {
            LOGE("❌ 颜色空间转换失败: %d", ret);
            return false;
        }
    }
    
    // 智能检测像素格式
    AVPixelFormat detectPixelFormat(AVFrame* frame) {
        if (frame->format != 23) {
            return (AVPixelFormat)frame->format;
        }
        
        // MediaCodec格式检测
        if (frame->linesize[1] == frame->linesize[0] && frame->data[1] && !frame->data[2]) {
            return AV_PIX_FMT_NV12;
        } else if (frame->linesize[1] == frame->linesize[0]/2 && frame->data[1] && frame->data[2]) {
            return AV_PIX_FMT_YUV420P;
        } else {
            return AV_PIX_FMT_NV21; // Android默认
        }
    }
    
    // 更新SwsContext
    bool updateSwsContext(AVFrame* frame, AVPixelFormat input_format) {
        // 严格的输入验证
        if (!frame || frame->width <= 0 || frame->height <= 0) {
            LOGE("❌ 无效的帧参数: frame=%p, width=%d, height=%d", 
                 frame, frame ? frame->width : 0, frame ? frame->height : 0);
            return false;
        }
        
        // 检查像素格式是否有效
        if (input_format == AV_PIX_FMT_NONE || input_format < 0) {
            LOGE("❌ 无效的像素格式: %d", input_format);
            return false;
        }
        
        // 检查尺寸是否合理
        if (frame->width > 4096 || frame->height > 4096) {
            LOGE("❌ 帧尺寸过大: %dx%d", frame->width, frame->height);
            return false;
        }
        
        int dst_width = frame->width;
        int dst_height = frame->height;
        
        // 检查是否需要重建SwsContext
        if (sws_ctx && 
            cached_src_width == frame->width && 
            cached_src_height == frame->height &&
            cached_src_format == input_format &&
            cached_dst_width == dst_width &&
            cached_dst_height == dst_height) {
            return true; // 无需重建
        }
        
        // 释放旧的SwsContext
        if (sws_ctx) {
            sws_freeContext(sws_ctx);
            sws_ctx = nullptr;
        }
        
        // 创建新的SwsContext - 添加错误检查
        LOGD("🔄 创建SwsContext: %dx%d %s->RGBA", 
             frame->width, frame->height, av_get_pix_fmt_name(input_format));
        
        sws_ctx = sws_getContext(
            frame->width, frame->height, input_format,
            dst_width, dst_height, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        
        if (!sws_ctx) {
            LOGE("❌ 创建SwsContext失败: %dx%d %s->RGBA", 
                 frame->width, frame->height, av_get_pix_fmt_name(input_format));
            return false;
        }
        
        // 更新缓存参数
        cached_src_width = frame->width;
        cached_src_height = frame->height;
        cached_src_format = input_format;
        cached_dst_width = dst_width;
        cached_dst_height = dst_height;
        
        LOGI("✅ SwsContext创建成功: %dx%d %s->RGBA", 
             frame->width, frame->height, av_get_pix_fmt_name(input_format));
        
        return true;
    }
};

// 全局渲染器实例
static UltraLowLatencyRenderer* g_renderer = nullptr;
static std::mutex g_renderer_mutex;

// FFmpeg管理类
class FFmpegManager {
private:
    static FFmpegManager* instance;
    static std::mutex mutex_;
    bool initialized;
    void* ffmpeg_handle;

    FFmpegManager() : initialized(false), ffmpeg_handle(nullptr) {}

public:
    static FFmpegManager* getInstance() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (instance == nullptr) {
            instance = new FFmpegManager();
        }
        return instance;
    }

    bool initializeFFmpeg() {
        if (initialized) {
            return true;
        }

#if FFMPEG_FOUND
        LOGI("Initializing FFmpeg...");

        // 初始化FFmpeg网络模块
        avformat_network_init();

        // 注册所有编解码器和格式
        // 注意：在新版本FFmpeg中，这些函数已经被弃用，因为注册是自动的
        // av_register_all(); // 已弃用
        // avcodec_register_all(); // 已弃用

        initialized = true;
        LOGI("✅ FFmpeg initialized successfully");
        return true;
#else
        LOGE("❌ FFmpeg not compiled");
        return false;
#endif
    }

    void cleanupFFmpeg() {
        if (!initialized) {
            return;
        }

#if FFMPEG_FOUND
        LOGI("Cleaning up FFmpeg...");
        avformat_network_deinit();
        initialized = false;
        LOGI("✅ FFmpeg cleanup completed");
#endif
    }

    bool isInitialized() const {
        return initialized;
    }

    std::string getVersion() {
#if FFMPEG_FOUND
        if (!initialized) {
            return "FFmpeg not initialized";
        }
        return std::string("FFmpeg ") + av_version_info();
#else
        return "FFmpeg not available";
#endif
    }
};

// 静态成员定义
FFmpegManager* FFmpegManager::instance = nullptr;
std::mutex FFmpegManager::mutex_;

// 全局变量 - 移到条件编译块外，确保总是可用
static bool hardware_decode_enabled = true;
static bool hardware_decode_available = false;
static bool rtsp_connected = false;
static bool rtsp_recording = false;
static int processed_frame_count = 0;
static long total_decode_time = 0;
static int video_stream_index = -1;

// Surface和渲染相关变量
static ANativeWindow* native_window = nullptr;
static ANativeWindow_Buffer window_buffer;
static bool surface_locked = false;  // 跟踪Surface锁定状态
static bool surface_valid = false;   // 跟踪Surface有效性
static bool surface_ready = false;   // 跟踪Surface是否准备好渲染
static std::mutex surface_mutex;     // Surface访问保护
static std::mutex frame_processing_mutex; // 帧处理保护

// 简化的Surface管理 - 不绑定Activity生命周期
static std::atomic<bool> surface_being_recreated(false); // Surface正在重建
static std::condition_variable surface_cv; // Surface状态变化通知


#if FFMPEG_FOUND
// FFmpeg相关的全局变量 - 只有在FFmpeg可用时才声明
static AVFormatContext* rtsp_input_ctx = nullptr;
static AVFormatContext* rtsp_output_ctx = nullptr;
static AVCodecContext* decoder_ctx = nullptr;
static SwsContext* sws_ctx = nullptr;
static AVFrame* frame = nullptr;
static AVFrame* frame_rgba = nullptr;
static AVRational video_stream_timebase = {1, 1000000}; // 默认微秒时间基准
#endif

// FFmpeg初始化和清理函数
static bool initializeFFmpegInternal() {
    return FFmpegManager::getInstance()->initializeFFmpeg();
}

// 渲染帧到Surface的辅助函数
#if FFMPEG_FOUND
static void renderFrameToSurface(AVFrame* frame) {
    // 线程安全的Surface有效性检查
    std::lock_guard<std::mutex> lock(surface_mutex);

    // 检查Surface是否正在重建
    if (surface_being_recreated.load()) {
        static int recreating_count = 0;
        if (recreating_count++ % 50 == 0) {
            LOGD("🔄 Surface正在重建，跳过渲染 (第%d次)", recreating_count);
        }
        return;
    }

    if (!native_window || !frame || !surface_valid || !surface_ready) {
        static int invalid_surface_count = 0;
        if (invalid_surface_count++ % 50 == 0) {
            LOGW("⚠️ Surface无效或帧为空: native_window=%p, frame=%p, surface_valid=%s, surface_ready=%s (第%d次)",
                 native_window, frame, surface_valid ? "true" : "false", surface_ready ? "true" : "false", invalid_surface_count);
        }
        return;
    }

    // 双重检查Surface有效性
    if (surface_locked) {
        static int locked_count = 0;
        if (locked_count++ % 30 == 0) {
            LOGW("⚠️ Surface已被锁定，跳过渲染 (第%d次)", locked_count);
        }
        return;
    }

    // 基本帧数据验证
    if (frame->width <= 0 || frame->height <= 0 || frame->format < 0) {
        static int render_invalid_count = 0;
        if (render_invalid_count++ % 10 == 0) {
            LOGE("❌ 无效帧尺寸或格式: size=%dx%d, format=%d (渲染函数第%d次)",
                 frame->width, frame->height, frame->format, render_invalid_count);
        }
        return;
    }

    // 调试：记录进入渲染函数的帧信息
    static int render_entry_count = 0;
    if (render_entry_count++ % 30 == 0) {
        LOGD("🎨 进入渲染函数: %dx%d, format=%d, data[0]=%p (第%d次)",
             frame->width, frame->height, frame->format, frame->data[0], render_entry_count);
    }

    // 对于硬件解码，data[0]可能为空，这是正常的
    if (!hardware_decode_available && !frame->data[0]) {
        LOGE("❌ 软件解码帧缺少数据: data[0]=%p", frame->data[0]);
        return;
    }

    // 减少日志输出频率
    static int render_debug_count = 0;
    if (render_debug_count++ % 30 == 0) {
        LOGD("🎬 渲染帧: %dx%d, format=%d, data[0]=%p, data[1]=%p, data[3]=%p",
             frame->width, frame->height, frame->format,
             frame->data[0], frame->data[1], frame->data[3]);
    }

    // 检查是否是MediaCodec硬件解码器输出
    if (frame->format == 23) { // MediaCodec硬件格式
        // 检查是否有MediaCodec缓冲区引用
        if (frame->data[3] != nullptr) {
            // 真正的MediaCodec硬件Surface输出 - 直接渲染到Surface
            int ret = av_mediacodec_release_buffer((AVMediaCodecBuffer*)frame->data[3], 1);
            if (ret < 0) {
                LOGE("❌ MediaCodec缓冲区释放失败: %d", ret);
            }
            return;
        }
        // MediaCodec回退到CPU模式，继续下面的软件渲染
    }

    // 只在第一次或尺寸变化时设置缓冲区几何
    static int last_width = 0, last_height = 0;
    if (last_width != frame->width || last_height != frame->height) {
        int ret = ANativeWindow_setBuffersGeometry(native_window, frame->width, frame->height, WINDOW_FORMAT_RGBA_8888);
        if (ret != 0) {
            LOGE("❌ 设置Surface缓冲区几何失败: %d", ret);
            return;
        }
        last_width = frame->width;
        last_height = frame->height;
        LOGI("✅ 设置Surface缓冲区: %dx%d", frame->width, frame->height);
    }

    // MediaCodec格式23的特殊处理 - 根据实际数据布局智能检测格式
    AVPixelFormat input_format;

    if (frame->format == 23) {
        // 分析MediaCodec格式23的实际数据布局
        static int format_debug_count = 0;
        if (format_debug_count++ % 30 == 0) {
            LOGD("🔍 MediaCodec格式23分析: %dx%d, linesize=[%d,%d,%d], data=[%p,%p,%p]",
                 frame->width, frame->height,
                 frame->linesize[0], frame->linesize[1], frame->linesize[2],
                 frame->data[0], frame->data[1], frame->data[2]);
        }

        // 智能格式检测：基于linesize和data指针的布局
        if (frame->linesize[1] == frame->linesize[0] && frame->data[1] != nullptr && frame->data[2] == nullptr) {
            // linesize[1] == linesize[0] 且只有data[0]和data[1]，这是NV12格式
            input_format = AV_PIX_FMT_NV12;
            if (format_debug_count % 30 == 0) {
                LOGI("🎯 检测到NV12格式 (linesize[1]==linesize[0])");
            }
        } else if (frame->linesize[1] == frame->linesize[0]/2 && frame->data[1] != nullptr && frame->data[2] != nullptr) {
            // linesize[1] == linesize[0]/2 且有data[0]、data[1]、data[2]，这是YUV420P格式
            input_format = AV_PIX_FMT_YUV420P;
            if (format_debug_count % 30 == 0) {
                LOGI("🎯 检测到YUV420P格式 (linesize[1]==linesize[0]/2)");
            }
        } else if (frame->data[1] != nullptr && frame->data[2] == nullptr) {
            // 只有data[0]和data[1]，默认NV21（Android常用）
            input_format = AV_PIX_FMT_NV21;
            if (format_debug_count % 30 == 0) {
                LOGI("🎯 默认使用NV21格式 (Android标准)");
            }
        } else {
            // 回退到YUV420P
            input_format = AV_PIX_FMT_YUV420P;
            if (format_debug_count % 30 == 0) {
                LOGI("🎯 回退到YUV420P格式");
            }
        }
    } else {
        input_format = (AVPixelFormat)frame->format;
    }

    // 线程安全的SwsContext管理
    static SwsContext* cached_sws_ctx = nullptr;
    static int cached_width = 0, cached_height = 0;
    static AVPixelFormat cached_format = AV_PIX_FMT_NONE;
    static std::mutex sws_mutex;

    SwsContext* current_sws_ctx = nullptr;

    // 线程安全地获取SwsContext - 添加Surface状态检查
    {
        std::lock_guard<std::mutex> sws_lock(sws_mutex);

        // 检查Surface状态，防止在Surface重建期间操作SwsContext
        if (surface_being_recreated.load() || !surface_valid) {
            LOGD("🛑 Surface重建中或无效，跳过SwsContext操作");
            return;
        }

        // 更新SwsContext（如果需要）
        if (!cached_sws_ctx || cached_width != frame->width || cached_height != frame->height || cached_format != input_format) {

            // 释放旧的SwsContext
            if (cached_sws_ctx) {
                sws_freeContext(cached_sws_ctx);
                cached_sws_ctx = nullptr;
            }

            // 按优先级尝试创建SwsContext
            const AVPixelFormat try_formats[] = {
                    input_format,        // 首选检测到的格式
                    AV_PIX_FMT_NV21,    // Android标准格式
                    AV_PIX_FMT_NV12,    // 通用NV12格式
                    AV_PIX_FMT_YUV420P  // 通用YUV420P格式
            };

            bool success = false;
            for (int i = 0; i < 4; i++) {
                // 跳过重复的格式
                if (i > 0 && try_formats[i] == input_format) {
                    continue;
                }

                cached_sws_ctx = sws_getContext(
                        frame->width, frame->height, try_formats[i],
                        frame->width, frame->height, AV_PIX_FMT_RGBA,
                        SWS_BILINEAR, nullptr, nullptr, nullptr);

                if (cached_sws_ctx) {
                    input_format = try_formats[i]; // 更新实际使用的格式
                    cached_format = input_format;
                    success = true;
                    LOGD("🔄 SwsContext创建成功: %dx%d, %s->RGBA",
                         frame->width, frame->height, av_get_pix_fmt_name(input_format));
                    break;
                } else {
                    LOGW("⚠️ SwsContext创建失败: %s", av_get_pix_fmt_name(try_formats[i]));
                }
            }

            if (!success) {
                LOGE("❌ 所有格式都无法创建SwsContext");
                return;
            }

            cached_width = frame->width;
            cached_height = frame->height;
        }

        current_sws_ctx = cached_sws_ctx;
    }

    // 检查SwsContext是否有效
    if (!current_sws_ctx) {
        LOGE("❌ SwsContext无效，跳过渲染");
        return;
    }

    // 再次检查Surface状态（SwsContext获取后可能Surface已变化）
    if (!surface_valid || !native_window) {
        LOGW("⚠️ Surface在SwsContext获取后变为无效，跳过渲染");
        return;
    }

    // 超低延迟渲染：激进的跳帧策略
    static auto last_render_time = std::chrono::steady_clock::now();
    auto current_time = std::chrono::steady_clock::now();
    auto time_since_last = std::chrono::duration_cast<std::chrono::milliseconds>(
            current_time - last_render_time).count();

    // 超智能跳帧策略：基于实际性能动态调整
    static int consecutive_slow_renders = 0;
    static int consecutive_fast_renders = 0;
    static int adaptive_threshold = 30; // 动态调整的跳帧阈值
    static auto last_threshold_update = std::chrono::steady_clock::now();

    // 性能检测和阈值调整
    if (time_since_last > 50) {
        // 渲染很慢
        consecutive_slow_renders++;
        consecutive_fast_renders = 0;

        if (consecutive_slow_renders > 3) {
            adaptive_threshold = std::max(15, adaptive_threshold - 2); // 降低阈值，允许更频繁渲染
        }
    } else if (time_since_last < 20) {
        // 渲染很快
        consecutive_fast_renders++;
        consecutive_slow_renders = 0;

        if (consecutive_fast_renders > 5) {
            adaptive_threshold = std::min(35, adaptive_threshold + 1); // 提高阈值，减少不必要渲染
        }
    } else {
        // 渲染正常
        consecutive_slow_renders = 0;
        consecutive_fast_renders = 0;
    }

    // 定期重置阈值（避免长期偏移）
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_threshold_update).count() > 10) {
        adaptive_threshold = 30; // 重置为默认值
        last_threshold_update = now;
    }

    // 应用智能跳帧策略
    if (time_since_last < adaptive_threshold) {
        static int skip_count = 0;
        if (skip_count++ % 60 == 0) {
            LOGD("🧠 智能跳帧: %lldms < %dms (慢渲染:%d, 快渲染:%d)",
                 time_since_last, adaptive_threshold, consecutive_slow_renders, consecutive_fast_renders);
        }
        return;
    }

    // 最终Surface安全检查
    if (surface_locked || !surface_valid || !native_window) {
        static int final_check_fail_count = 0;
        if (final_check_fail_count++ % 30 == 0) {
            LOGW("⚠️ 最终检查失败: locked=%s, valid=%s, window=%p (第%d次)",
                 surface_locked ? "true" : "false",
                 surface_valid ? "true" : "false",
                 native_window, final_check_fail_count);
        }
        return;
    }

    // 尝试非阻塞锁定
    ANativeWindow_Buffer buffer;
    int lock_ret = ANativeWindow_lock(native_window, &buffer, nullptr);
    if (lock_ret != 0) {
        static int lock_fail_count = 0;
        if (lock_fail_count++ % 30 == 0) {
            LOGW("⚠️ ANativeWindow_lock失败: %d，可能Surface已销毁 (第%d次)", lock_ret, lock_fail_count);
        }
        // Surface可能已经无效，标记为无效
        surface_valid = false;
        return;
    }

    // 成功锁定，标记状态
    surface_locked = true;

    // 计算目标参数
    int dst_stride = buffer.stride * 4;
    uint8_t* dst_data[4] = {(uint8_t*)buffer.bits, nullptr, nullptr, nullptr};
    int dst_linesize[4] = {dst_stride, 0, 0, 0};

    // 最后一次检查：确保所有指针有效且Surface未被重建
    if (!current_sws_ctx || !frame->data[0] || !dst_data[0] || !surface_valid || surface_being_recreated.load()) {
        LOGE("❌ sws_scale前检查失败: sws_ctx=%p, frame_data=%p, dst_data=%p, surface_valid=%s, recreating=%s",
             current_sws_ctx, frame->data[0], dst_data[0], surface_valid ? "true" : "false",
             surface_being_recreated.load() ? "true" : "false");
        ANativeWindow_unlockAndPost(native_window);
        surface_locked = false;
        return;
    }

    // 直接转换到window buffer - 使用线程安全的SwsContext
    // 再次检查Surface状态，这是最后的保护
    if (surface_being_recreated.load()) {
        LOGE("❌ sws_scale执行前Surface被重建，中止");
        ANativeWindow_unlockAndPost(native_window);
        surface_locked = false;
        return;
    }

    int ret = sws_scale(current_sws_ctx, frame->data, frame->linesize, 0, frame->height,
                        dst_data, dst_linesize);

    if (ret > 0) {
        // 成功转换，直接显示
        if (ANativeWindow_unlockAndPost(native_window) == 0) {
            surface_locked = false;  // 标记Surface已解锁
            last_render_time = current_time;

            // 计算实际渲染帧率（每30帧输出一次）
            static int render_count = 0;
            static auto fps_start_time = current_time;

            render_count++;
            if (render_count % 30 == 0) {
                auto total_time = std::chrono::duration_cast<std::chrono::microseconds>(
                        current_time - fps_start_time).count();

                if (total_time > 0) {
                    float render_fps = 30000000.0f / total_time; // fps
                    LOGD("🎨 实际渲染FPS: %.1f", render_fps);
                }
                fps_start_time = current_time;
            }
        } else {
            surface_locked = false;  // 即使失败也要标记解锁
            LOGE("❌ ANativeWindow_unlockAndPost失败");
        }
    } else {
        // 转换失败，解锁buffer
        ANativeWindow_unlockAndPost(native_window);
        surface_locked = false;  // 标记Surface已解锁

        // 减少错误日志输出频率
        static int error_count = 0;
        if (error_count++ % 10 == 0) {
            LOGE("❌ 颜色空间转换失败: %d (格式:%s)", ret, av_get_pix_fmt_name(input_format));
        }
    }
}
#endif

static void cleanupFFmpegInternal() {
    FFmpegManager::getInstance()->cleanupFFmpeg();

#if FFMPEG_FOUND
    // 清理RTSP相关资源
    if (rtsp_input_ctx) {
        avformat_close_input(&rtsp_input_ctx);
        rtsp_input_ctx = nullptr;
    }

    if (rtsp_output_ctx) {
        if (!(rtsp_output_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&rtsp_output_ctx->pb);
        }
        avformat_free_context(rtsp_output_ctx);
        rtsp_output_ctx = nullptr;
    }

    if (decoder_ctx) {
        avcodec_free_context(&decoder_ctx);
        decoder_ctx = nullptr;
    }

    if (sws_ctx) {
        sws_freeContext(sws_ctx);
        sws_ctx = nullptr;
    }

    if (frame) {
        av_frame_free(&frame);
        frame = nullptr;
    }

    if (frame_rgba) {
        av_frame_free(&frame_rgba);
        frame_rgba = nullptr;
    }
#endif

    // 清理native window
    {
        std::lock_guard<std::mutex> lock(surface_mutex);
        surface_valid = false;
        surface_locked = false;
        if (native_window) {
            ANativeWindow_release(native_window);
            native_window = nullptr;
        }
    }

    // 重置状态 - 这些变量现在总是可用
    rtsp_connected = false;
    rtsp_recording = false;
    processed_frame_count = 0;
    total_decode_time = 0;
    video_stream_index = -1;
}

// JNI方法实现

extern "C" JNIEXPORT jstring JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_stringFromJNI(JNIEnv *env, jobject /* thiz */) {
    LOGI("🔧 stringFromJNI被调用，FFMPEG_FOUND=%d", FFMPEG_FOUND);
    std::string hello = "Hello from C++, FFMPEG_FOUND=" + std::to_string(FFMPEG_FOUND);
    return env->NewStringUTF(hello.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_getFFmpegVersion(JNIEnv *env, jobject /* thiz */) {
    // 确保FFmpeg已初始化
    if (!initializeFFmpegInternal()) {
        return env->NewStringUTF("FFmpeg initialization failed");
    }

    std::string version = FFmpegManager::getInstance()->getVersion();
    return env->NewStringUTF(version.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_getVideoInfo(JNIEnv *env, jobject /* thiz */, jstring jpath) {
#if FFMPEG_FOUND
    if (!initializeFFmpegInternal()) {
        return env->NewStringUTF("FFmpeg initialization failed");
    }

    if (!jpath) {
        return env->NewStringUTF("Invalid file path");
    }

    const char *path = env->GetStringUTFChars(jpath, nullptr);
    if (!path) {
        return env->NewStringUTF("Cannot get file path");
    }

    std::string info = "Video Info:\n";
    info += "File: " + std::string(path) + "\n";

    // 尝试打开文件获取信息
    AVFormatContext *fmt_ctx = nullptr;
    int ret = avformat_open_input(&fmt_ctx, path, nullptr, nullptr);

    if (ret >= 0) {
        ret = avformat_find_stream_info(fmt_ctx, nullptr);
        if (ret >= 0) {
            info += "Duration: " + std::to_string(fmt_ctx->duration / AV_TIME_BASE) + " seconds\n";
            info += "Bitrate: " + std::to_string(fmt_ctx->bit_rate) + " bps\n";
            info += "Streams: " + std::to_string(fmt_ctx->nb_streams) + "\n";

            for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
                AVStream *stream = fmt_ctx->streams[i];
                AVCodecParameters *codecpar = stream->codecpar;

                if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                    info += "Video: " + std::string(avcodec_get_name(codecpar->codec_id));
                    info += " " + std::to_string(codecpar->width) + "x" + std::to_string(codecpar->height) + "\n";
                } else if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                    info += "Audio: " + std::string(avcodec_get_name(codecpar->codec_id));
                    info += " " + std::to_string(codecpar->sample_rate) + "Hz\n";
                }
            }
        } else {
            info += "Failed to get stream info\n";
        }
        avformat_close_input(&fmt_ctx);
    } else {
        info += "Failed to open file\n";
    }

    env->ReleaseStringUTFChars(jpath, path);
    return env->NewStringUTF(info.c_str());
#else
    return env->NewStringUTF("FFmpeg not compiled - please build FFmpeg first");
#endif
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_convertVideo(JNIEnv *env, jobject /* thiz */,
                                                     jstring input_path, jstring output_path) {
#if FFMPEG_FOUND
    if (!initializeFFmpegInternal()) {
        LOGE("FFmpeg initialization failed");
        return JNI_FALSE;
    }

    if (!input_path || !output_path) {
        LOGE("Invalid input or output path");
        return JNI_FALSE;
    }

    const char *input = env->GetStringUTFChars(input_path, nullptr);
    const char *output = env->GetStringUTFChars(output_path, nullptr);

    if (!input || !output) {
        if (input) env->ReleaseStringUTFChars(input_path, input);
        if (output) env->ReleaseStringUTFChars(output_path, output);
        LOGE("Cannot get path strings");
        return JNI_FALSE;
    }

    LOGI("Convert video: %s -> %s", input, output);

    // TODO: Implement actual video conversion using FFmpeg
    bool success = true; // Placeholder

    env->ReleaseStringUTFChars(input_path, input);
    env->ReleaseStringUTFChars(output_path, output);

    return success ? JNI_TRUE : JNI_FALSE;
#else
    LOGE("FFmpeg not available");
    return JNI_FALSE;
#endif
}

// RTSP相关方法
// 超低延迟解码器初始化函数
#if FFMPEG_FOUND
static int initUltraLowLatencyDecoder(AVStream* stream) {
    AVCodecID codec_id = stream->codecpar->codec_id;
    const char *codec_name = avcodec_get_name(codec_id);

    LOGI("🚀 初始化超低延迟解码器: %s (ID: %d)", codec_name, codec_id);

    const AVCodec *decoder = nullptr;

    // 优先尝试硬件解码器（更低延迟）
    if (hardware_decode_enabled) {
        if (codec_id == AV_CODEC_ID_H264) {
            decoder = avcodec_find_decoder_by_name("h264_mediacodec");
            if (decoder) {
                LOGI("✅ 找到H.264硬件解码器");
                hardware_decode_available = true;
            }
        } else if (codec_id == AV_CODEC_ID_HEVC) {
            // HEVC硬件解码器支持检查
            decoder = avcodec_find_decoder_by_name("hevc_mediacodec");
            if (decoder) {
                LOGI("✅ 找到HEVC硬件解码器");
                hardware_decode_available = true;
            } else {
                LOGW("⚠️ 设备不支持HEVC硬件解码，将使用软件解码");
            }
        }
    }

    // 如果硬件解码器不可用，使用软件解码器
    if (!decoder) {
        decoder = avcodec_find_decoder(codec_id);
        if (decoder) {
            LOGI("✅ 使用软件解码器: %s", decoder->name);
            hardware_decode_available = false;
        } else {
            LOGE("❌ 未找到适合的解码器");
            return -1;
        }
    }

    // 分配解码器上下文
    decoder_ctx = avcodec_alloc_context3(decoder);
    if (!decoder_ctx) {
        LOGE("❌ 分配解码器上下文失败");
        return -1;
    }

    // 复制编解码器参数
    int ret = avcodec_parameters_to_context(decoder_ctx, stream->codecpar);
    if (ret < 0) {
        LOGE("❌ 复制编解码器参数失败: %d", ret);
        avcodec_free_context(&decoder_ctx);
        decoder_ctx = nullptr;
        return -1;
    }

    // 关键：设置超低延迟选项
    decoder_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;           // 低延迟标志
    decoder_ctx->flags2 |= AV_CODEC_FLAG2_FAST;             // 快速解码
    decoder_ctx->thread_count = 1;                          // 单线程避免帧重排序
    decoder_ctx->thread_type = FF_THREAD_SLICE;             // 切片线程
    decoder_ctx->delay = 0;                                 // 最小解码延迟
    decoder_ctx->has_b_frames = 0;                         // 禁用B帧
    decoder_ctx->max_b_frames = 0;                         // 禁用B帧
    decoder_ctx->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL; // 允许非标准优化
    decoder_ctx->workaround_bugs = FF_BUG_AUTODETECT;      // 自动修复已知问题

    // 设置硬件解码器参数（在外层声明）
    AVDictionary *hw_opts = nullptr;

    // 硬件解码器特殊配置
    if (hardware_decode_available) {
        LOGI("🔧 应用硬件解码器低延迟配置");

        // 设置基本的MediaCodec选项
        av_dict_set(&hw_opts, "delay_flush", "1", 0);   // 低延迟刷新
        av_dict_set(&hw_opts, "threads", "1", 0);       // 单线程

        // 注意：不在这里设置Surface，而是在解码器打开后通过官方API设置
        if (native_window) {
            LOGI("🖥️ Surface已准备就绪，将在解码器打开后配置: %p", native_window);
        } else {
            LOGW("⚠️ 警告：未设置Surface，将使用CPU内存输出");
        }
    } else {
        // 软件解码器额外优化
        decoder_ctx->skip_frame = AVDISCARD_NONREF;         // 跳过非参考帧
        decoder_ctx->skip_idct = AVDISCARD_BIDIR;           // 跳过双向预测的IDCT
        decoder_ctx->skip_loop_filter = AVDISCARD_BIDIR;    // 跳过环路滤波
    }

    // 打开解码器（传递硬件选项）
    AVDictionary *open_opts = nullptr;
    if (hardware_decode_available && hw_opts) {
        // 复制硬件选项用于打开解码器
        av_dict_copy(&open_opts, hw_opts, 0);
        av_dict_free(&hw_opts);
    }

    ret = avcodec_open2(decoder_ctx, decoder, &open_opts);
    if (open_opts) {
        av_dict_free(&open_opts);
    }

    if (ret < 0) {
        LOGE("❌ 打开解码器失败: %d", ret);
        avcodec_free_context(&decoder_ctx);
        decoder_ctx = nullptr;
        return -1;
    }

    // 解码器成功打开后，配置MediaCodec Surface输出
    if (hardware_decode_available && native_window) {
        LOGI("🖥️ 配置MediaCodec Surface输出...");

        // 关键修复：确保Surface没有被其他producer占用
        // 先尝试释放任何现有的Surface连接
        LOGI("🔧 准备Surface连接状态...");

        // 检查Surface是否被CPU锁定，如果是则等待或强制解锁
        if (surface_locked) {
            LOGW("⚠️ Surface当前被CPU锁定，尝试等待解锁...");
            // 等待一段时间让CPU渲染完成
            int wait_count = 0;
            while (surface_locked && wait_count < 10) {
                usleep(5000); // 等待5ms
                wait_count++;
            }

            if (surface_locked) {
                LOGW("⚠️ Surface仍被锁定，这可能导致硬件解码失败");
            } else {
                LOGI("✅ Surface已解锁，可以尝试硬件解码");
            }
        }

        // 延迟一小段时间确保Surface准备完成
        usleep(10000); // 10ms延迟

        // 使用FFmpeg官方的MediaCodec Surface API
        int surface_ret = av_mediacodec_default_init(decoder_ctx, nullptr, native_window);
        if (surface_ret >= 0) {
            LOGI("✅ MediaCodec Surface配置成功 - 硬件直接渲染");
        } else {
            // 详细的错误分析
            LOGW("⚠️ MediaCodec Surface配置失败(ret=%d)", surface_ret);

            // 分析具体错误原因
            if (surface_ret == -22 || surface_ret == -542398533) {
                LOGW("   - Surface连接冲突：Surface已被其他producer占用");
                LOGW("   - 这通常发生在Surface被CPU渲染占用时");
                LOGW("   - 建议：确保Surface未被ANativeWindow_lock占用");
            } else if (codec_id == AV_CODEC_ID_HEVC) {
                LOGW("   - HEVC硬件解码可能不稳定，建议使用H.264");
                LOGW("   - 某些设备的HEVC MediaCodec支持有限");
            }
            LOGW("   - 回退到CPU渲染模式");

            // 强制禁用硬件解码标志，确保后续使用CPU路径
            hardware_decode_available = false;

            // 重新创建软件解码器
            avcodec_free_context(&decoder_ctx);
            decoder = avcodec_find_decoder(codec_id);
            if (decoder) {
                LOGI("🔄 重新创建软件解码器: %s", decoder->name);
                decoder_ctx = avcodec_alloc_context3(decoder);
                if (decoder_ctx) {
                    ret = avcodec_parameters_to_context(decoder_ctx, stream->codecpar);
                    if (ret < 0) {
                        LOGE("❌ 软件解码器参数设置失败: %d", ret);
                        avcodec_free_context(&decoder_ctx);
                        return -1;
                    }

                    // 软件解码器优化设置
                    decoder_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
                    decoder_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
                    decoder_ctx->thread_count = 1;
                    decoder_ctx->thread_type = FF_THREAD_SLICE;
                    decoder_ctx->delay = 0;
                    decoder_ctx->has_b_frames = 0;
                    decoder_ctx->max_b_frames = 0;
                    decoder_ctx->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
                    decoder_ctx->workaround_bugs = FF_BUG_AUTODETECT;

                    // 软件解码器额外优化
                    decoder_ctx->skip_frame = AVDISCARD_NONREF;
                    decoder_ctx->skip_idct = AVDISCARD_BIDIR;
                    decoder_ctx->skip_loop_filter = AVDISCARD_BIDIR;

                    ret = avcodec_open2(decoder_ctx, decoder, nullptr);
                    if (ret >= 0) {
                        LOGI("✅ 软件解码器重新初始化成功: %s", decoder->name);
                        LOGI("   - 解码器能力: %s", decoder->long_name ? decoder->long_name : "未知");
                        LOGI("   - 输入格式: %s (%dx%d)", avcodec_get_name(codec_id),
                             decoder_ctx->width, decoder_ctx->height);
                    } else {
                        LOGE("❌ 软件解码器打开失败: %d", ret);
                        avcodec_free_context(&decoder_ctx);
                        return -1;
                    }
                } else {
                    LOGE("❌ 软件解码器上下文分配失败");
                    return -1;
                }
            } else {
                LOGE("❌ 未找到软件解码器");
                return -1;
            }
        }
    } else if (hardware_decode_available) {
        LOGW("⚠️ 硬件解码器已打开但Surface未设置，将使用CPU渲染");
        hardware_decode_available = false;
    }

    LOGI("✅ 超低延迟解码器初始化成功");
    return 0;
}
#endif

extern "C" JNIEXPORT jboolean JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_openRtspStream(JNIEnv *env, jobject /* thiz */, jstring rtsp_url) {
#if FFMPEG_FOUND
    if (!initializeFFmpegInternal()) {
        LOGE("FFmpeg initialization failed");
        return JNI_FALSE;
    }

    if (!rtsp_url) {
        LOGE("Invalid RTSP URL");
        return JNI_FALSE;
    }

    const char *url = env->GetStringUTFChars(rtsp_url, nullptr);
    if (!url) {
        LOGE("Cannot get RTSP URL");
        return JNI_FALSE;
    }

    LOGI("🚀 使用超低延迟播放核心打开RTSP流: %s", url);

    // 线程安全地初始化播放器
    {
        std::lock_guard<std::mutex> lock(g_player_mutex);
        
        // 清理旧的播放器
        if (g_player) {
            delete g_player;
            g_player = nullptr;
        }
        
        // 创建新的超低延迟播放器
        g_player = new UltraLowLatencyPlayer();
        if (!g_player->initialize(url)) {
            LOGE("❌ 超低延迟播放器初始化失败");
            delete g_player;
            g_player = nullptr;
            env->ReleaseStringUTFChars(rtsp_url, url);
            return JNI_FALSE;
        }
    }

    rtsp_connected = true;
    LOGI("✅ 超低延迟RTSP播放器启动成功");
    LOGI("📊 硬件解码: %s", g_player->isHardwareDecoding() ? "启用" : "禁用");

    env->ReleaseStringUTFChars(rtsp_url, url);
    return JNI_TRUE;
#else
    LOGE("FFmpeg not available");
    return JNI_FALSE;
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_getRtspStreamInfo(JNIEnv *env, jobject /* thiz */) {
#if FFMPEG_FOUND
    if (!rtsp_connected || !rtsp_input_ctx) {
        return env->NewStringUTF("RTSP stream not connected");
    }

    std::string info = "RTSP Stream Info:\n";
    info += "URL: " + std::string(rtsp_input_ctx->url ? rtsp_input_ctx->url : "unknown") + "\n";
    info += "Duration: " + (rtsp_input_ctx->duration != AV_NOPTS_VALUE ?
                            std::to_string(rtsp_input_ctx->duration / AV_TIME_BASE) + " seconds" : "Live stream") + "\n";
    info += "Bitrate: " + std::to_string(rtsp_input_ctx->bit_rate) + " bps\n";
    info += "Streams: " + std::to_string(rtsp_input_ctx->nb_streams) + "\n";
    info += "Hardware Decode: " + std::string(hardware_decode_available ? "Available" : "Not Available") + "\n";

    if (video_stream_index >= 0) {
        AVStream *stream = rtsp_input_ctx->streams[video_stream_index];
        AVCodecParameters *codecpar = stream->codecpar;
        info += "Video: " + std::string(avcodec_get_name(codecpar->codec_id));
        info += " " + std::to_string(codecpar->width) + "x" + std::to_string(codecpar->height) + "\n";
    }

    return env->NewStringUTF(info.c_str());
#else
    return env->NewStringUTF("FFmpeg not available");
#endif
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_prepareRecording(JNIEnv *env, jobject /* thiz */, jstring output_path) {
#if FFMPEG_FOUND
    LOGI("🔧 Native prepareRecording 开始");
    
    if (!output_path) {
        LOGE("🔧 output_path为空");
        return JNI_FALSE;
    }

    const char *path = env->GetStringUTFChars(output_path, nullptr);
    if (!path) {
        LOGE("🔧 无法获取路径字符串");
        return JNI_FALSE;
    }
    
    LOGI("🔧 录制路径: %s", path);

    // 线程安全地操作录制器
    LOGI("🔧 获取录制器锁");
    std::lock_guard<std::mutex> recorder_lock(g_recorder_mutex);
    
    // 清理旧录制器
    if (g_recorder) {
        LOGI("🔧 清理旧录制器");
        g_recorder->stop();
        delete g_recorder;
        g_recorder = nullptr;
    }
    
    // 创建新录制器并准备
    LOGI("🔧 创建新录制器");
    g_recorder = new ModernRecorder();
    bool success = g_recorder->prepare(path);
    LOGI("🔧 录制器准备结果: %s", success ? "成功" : "失败");
    
    env->ReleaseStringUTFChars(output_path, path);
    
    if (success) {
        LOGI("🔧 prepareRecording 成功");
        return JNI_TRUE;
    } else {
        LOGE("🔧 prepareRecording 失败，清理录制器");
        delete g_recorder;
        g_recorder = nullptr;
        return JNI_FALSE;
    }
#else
    LOGE("🔧 FFmpeg不可用");
    return JNI_FALSE;
#endif
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_startRtspRecording(JNIEnv *env, jobject /* thiz */, jstring output_path) {
#if FFMPEG_FOUND
    LOGI("🔧 Native startRtspRecording 开始");
    
    // 线程安全地操作录制器
    LOGI("🔧 获取录制器锁");
    std::lock_guard<std::mutex> recorder_lock(g_recorder_mutex);
    
    if (!g_recorder) {
        LOGE("🔧 录制器为空");
        return JNI_FALSE;
    }
    
    LOGI("🔧 使用默认视频参数（避免死锁）");
    // 避免死锁：不在持有recorder锁时获取player锁
    // 使用标准的1280x720分辨率和30fps
    int width = 1280, height = 720;
    AVRational framerate = {30, 1};
    LOGI("🔧 使用默认尺寸: %dx%d@%dfps", width, height, framerate.num);
    
    // 启动录制
    LOGI("🔧 启动录制器");
    bool success = g_recorder->start(width, height, framerate);
    LOGI("🔧 录制器启动结果: %s", success ? "成功" : "失败");
    
    if (success) {
        rtsp_recording = true;
        LOGI("🔧 startRtspRecording 成功");
        return JNI_TRUE;
    } else {
        LOGE("🔧 startRtspRecording 失败");
        return JNI_FALSE;
    }
#else
    LOGE("🔧 FFmpeg不可用");
    return JNI_FALSE;
#endif
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_stopRtspRecording(JNIEnv *env, jobject /* thiz */) {
#if FFMPEG_FOUND
    LOGI("🔧 Native stopRtspRecording 开始");
    std::lock_guard<std::mutex> recorder_lock(g_recorder_mutex);
    
    if (!g_recorder || !g_recorder->isActive()) {
        LOGI("🔧 录制器不存在或未激活，直接返回成功");
        rtsp_recording = false;
        return JNI_TRUE;
    }

    LOGI("🔧 调用录制器stop方法");
    g_recorder->stop();
    
    // 清理录制器
    LOGI("🔧 清理录制器");
    delete g_recorder;
    g_recorder = nullptr;
    
    rtsp_recording = false;
    LOGI("🔧 Native stopRtspRecording 完成");
    
    return JNI_TRUE;
#else
    return JNI_FALSE;
#endif
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_processRtspFrame(JNIEnv *env, jobject /* thiz */) {
#if FFMPEG_FOUND
    // 先处理播放器帧
    AVFrame* current_frame = nullptr;
    bool frame_processed = false;
    
    {
        std::lock_guard<std::mutex> player_lock(g_player_mutex);
        if (!g_player) {
            return JNI_FALSE;
        }

        frame_processed = g_player->processFrame();
        if (!frame_processed) {
            return JNI_FALSE;
        }

        current_frame = g_player->getCurrentFrame();
    }
    
    if (!current_frame) {
        return JNI_TRUE;
    }

    // 渲染帧
    {
        std::lock_guard<std::mutex> renderer_lock(g_renderer_mutex);
        if (g_renderer) {
            g_renderer->renderFrame(current_frame);
            processed_frame_count++;
        }
    }
    
    // 录制帧（避免死锁）
    {
        std::lock_guard<std::mutex> recorder_lock(g_recorder_mutex);
        
        if (g_recorder && g_recorder->isActive()) {
            g_recorder->writeFrame(current_frame);
        }
    }

    return JNI_TRUE;
#else
    return JNI_FALSE;
#endif
}

extern "C" JNIEXPORT void JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_closeRtspStream(JNIEnv *env, jobject /* thiz */) {
#if FFMPEG_FOUND
    if (rtsp_recording) {
        Java_com_jxj_CompileFfmpeg_MainActivity_stopRtspRecording(env, nullptr);
    }

    {
        std::lock_guard<std::mutex> lock(g_player_mutex);
        if (g_player) {
            delete g_player;
            g_player = nullptr;
        }
    }

    rtsp_connected = false;
    processed_frame_count = 0;
    total_decode_time = 0;
#endif
}

// 硬件解码控制方法
extern "C" JNIEXPORT void JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_setHardwareDecodeEnabled(JNIEnv *env, jobject /* thiz */, jboolean enabled) {
    hardware_decode_enabled = enabled;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_isHardwareDecodeEnabled(JNIEnv *env, jobject /* thiz */) {
    return hardware_decode_enabled ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_isHardwareDecodeAvailable(JNIEnv *env, jobject /* thiz */) {
    return hardware_decode_available ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_getDecoderInfo(JNIEnv *env, jobject /* thiz */) {
#if FFMPEG_FOUND
    std::string info = "超低延迟播放器信息:\n";
    info += "FFmpeg Initialized: " + std::string(FFmpegManager::getInstance()->isInitialized() ? "Yes" : "No") + "\n";
    
    // 线程安全地获取播放器信息
    {
        std::lock_guard<std::mutex> lock(g_player_mutex);
        if (g_player) {
            info += "播放器状态: 已初始化\n";
            info += "硬件解码: " + std::string(g_player->isHardwareDecoding() ? "启用" : "禁用") + "\n";
            
            int dropped_frames, slow_frames;
            g_player->getStats(dropped_frames, slow_frames);
            info += "丢弃帧数: " + std::to_string(dropped_frames) + "\n";
            info += "慢解码次数: " + std::to_string(slow_frames) + "\n";
        } else {
            info += "播放器状态: 未初始化\n";
        }
    }
    
    // 获取渲染器信息
    {
        std::lock_guard<std::mutex> lock(g_renderer_mutex);
        info += "渲染器状态: " + std::string(g_renderer ? "已初始化" : "未初始化") + "\n";
    }
    
    info += "RTSP连接: " + std::string(rtsp_connected ? "已连接" : "未连接") + "\n";
    info += "已处理帧数: " + std::to_string(processed_frame_count) + "\n";

    return env->NewStringUTF(info.c_str());
#else
    return env->NewStringUTF("FFmpeg not available");
#endif
}

// 性能监控方法
extern "C" JNIEXPORT jstring JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_getPerformanceStats(JNIEnv *env, jobject /* thiz */) {
#if FFMPEG_FOUND
    std::string stats = "Performance Stats:\n";
    stats += "Processed Frames: " + std::to_string(processed_frame_count) + "\n";
    stats += "Total Decode Time: " + std::to_string(total_decode_time) + " ms\n";

    if (processed_frame_count > 0) {
        long avg_time = total_decode_time / processed_frame_count;
        stats += "Average Decode Time: " + std::to_string(avg_time) + " ms\n";

        // 计算FPS（基于处理的帧数）
        if (total_decode_time > 0) {
            float fps = (float)processed_frame_count * 1000.0f / total_decode_time;
            stats += "Processing FPS: " + std::to_string(fps) + "\n";
        }
    }

    stats += "RTSP Connected: " + std::string(rtsp_connected ? "Yes" : "No") + "\n";
    stats += "Recording: " + std::string(rtsp_recording ? "Yes" : "No") + "\n";

    return env->NewStringUTF(stats.c_str());
#else
    return env->NewStringUTF("FFmpeg not available");
#endif
}

extern "C" JNIEXPORT void JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_resetPerformanceStats(JNIEnv *env, jobject /* thiz */) {
    processed_frame_count = 0;
    total_decode_time = 0;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_getAverageDecodeTime(JNIEnv *env, jobject /* thiz */) {
    if (processed_frame_count > 0) {
        return total_decode_time / processed_frame_count;
    }
    return 0;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_getProcessedFrameCount(JNIEnv *env, jobject /* thiz */) {
    return processed_frame_count;
}

// 新增方法：网络抖动后快速恢复
extern "C" JNIEXPORT void JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_flushBuffers(JNIEnv *env, jobject /* thiz */) {
#if FFMPEG_FOUND
    std::lock_guard<std::mutex> lock(g_player_mutex);
    if (g_player) {
        g_player->flushBuffers();
    }
#endif
}

// 移除Activity生命周期绑定 - 改为纯Surface状态管理



extern "C" JNIEXPORT void JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_setSurface(JNIEnv *env, jobject /* thiz */, jobject surface) {
    std::lock_guard<std::mutex> lock(g_renderer_mutex);
    
    if (!g_renderer) {
        g_renderer = new UltraLowLatencyRenderer();
    }

    ANativeWindow* native_window = nullptr;
    if (surface) {
        native_window = ANativeWindow_fromSurface(env, surface);
        if (!native_window) {
            g_surface_valid = false;
            g_rendering_paused = true;
            return;
        }
    } else {
        g_surface_valid = false;
        g_rendering_paused = true;
    }

    bool success = g_renderer->setSurface(native_window);
    if (!success) {
        g_surface_valid = false;
        g_rendering_paused = true;
    }
}

// JNI库加载和卸载
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /* reserved */) {
    JNIEnv* env;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }

    LOGI("🔧 JNI_OnLoad: 初始化FFmpeg包装器");
    
    // 输出编译时配置
    logCompileTimeConfig();

    // 初始化FFmpeg
    if (!initializeFFmpegInternal()) {
        LOGE("Failed to initialize FFmpeg in JNI_OnLoad");
        // 不返回错误，允许应用继续运行，但FFmpeg功能不可用
    }

    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* /* reserved */) {
    LOGI("JNI_OnUnload: 清理超低延迟播放核心...");
    
    // 清理播放器
    {
        std::lock_guard<std::mutex> lock(g_player_mutex);
        if (g_player) {
            delete g_player;
            g_player = nullptr;
        }
    }
    
    // 清理渲染器
    {
        std::lock_guard<std::mutex> lock(g_renderer_mutex);
        if (g_renderer) {
            delete g_renderer;
            g_renderer = nullptr;
        }
    }
    
    // 清理录制器
    {
        std::lock_guard<std::mutex> lock(g_recorder_mutex);
        if (g_recorder) {
            g_recorder->stop();
            delete g_recorder;
            g_recorder = nullptr;
        }
    }
    
    cleanupFFmpegInternal();
    LOGI("✅ 超低延迟播放核心清理完成");
} 