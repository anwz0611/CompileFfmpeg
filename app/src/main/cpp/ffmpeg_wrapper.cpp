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

#define LOG_TAG "FFmpegWrapper"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

// 检查FFmpeg是否可用
#ifndef FFMPEG_FOUND
#define FFMPEG_FOUND 0
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
}
#endif

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
            LOGD("🧠 智能跳帧: %ldms < %dms (慢渲染:%d, 快渲染:%d)", 
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
    std::string hello = "Hello from C++";
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
    
    LOGI("🚀 打开超低延迟RTSP流: %s", url);
    
    // 清理之前的连接
    if (rtsp_input_ctx) {
        avformat_close_input(&rtsp_input_ctx);
        rtsp_input_ctx = nullptr;
    }
    if (decoder_ctx) {
        avcodec_free_context(&decoder_ctx);
        decoder_ctx = nullptr;
    }
    
    // 创建输入上下文
    rtsp_input_ctx = avformat_alloc_context();
    if (!rtsp_input_ctx) {
        LOGE("❌ 分配输入上下文失败");
        env->ReleaseStringUTFChars(rtsp_url, url);
        return JNI_FALSE;
    }
    
    // 设置RTSP选项 - 激进超低延迟配置
    AVDictionary *options = nullptr;
    
    // ==== 网络和缓冲设置 ====
    av_dict_set(&options, "rtsp_transport", "tcp", 0);       // 使用TCP，更稳定
    av_dict_set(&options, "stimeout", "1000000", 0);         // 1秒超时（更快）
    av_dict_set(&options, "max_delay", "10000", 0);          // 最大延迟10ms（激进）
    av_dict_set(&options, "buffer_size", "65536", 0);        // 64KB缓冲区（最小）
    av_dict_set(&options, "fflags", "nobuffer+flush_packets", 0);  // 禁用缓冲+立即刷新
    av_dict_set(&options, "flags", "low_delay", 0);          // 低延迟模式
    
    // ==== 激进低延迟优化 ====
    av_dict_set(&options, "probesize", "8192", 0);           // 8KB探测（最小）
    av_dict_set(&options, "analyzeduration", "50000", 0);    // 50ms分析时间（激进）
    av_dict_set(&options, "sync", "ext", 0);                 // 外部同步
    av_dict_set(&options, "fpsprobesize", "1", 0);           // 最小fps探测
    
    LOGI("🔧 应用RTSP激进超低延迟配置...");
    LOGI("📋 配置详情: TCP传输, 1秒超时, 8KB探测, 50ms分析, 10ms最大延迟");
    LOGI("🔗 尝试连接: %s", url);
    
    // 打开RTSP流
    int ret = avformat_open_input(&rtsp_input_ctx, url, nullptr, &options);
    av_dict_free(&options);
    
    if (ret < 0) {
        char error_buf[256];
        av_strerror(ret, error_buf, sizeof(error_buf));
        LOGE("❌ 无法打开RTSP流: %s, 错误: %s (%d)", url, error_buf, ret);
        
        // 提供详细的错误诊断
        LOGE("🔍 错误诊断:");
        if (ret == -99 || ret == AVERROR(EADDRNOTAVAIL)) {
            LOGE("  - 网络地址不可用，请检查:");
            LOGE("    1. 设备是否连接到正确的网络");
            LOGE("    2. IP地址 192.168.144.25 是否可达");
            LOGE("    3. 端口 8554 是否开放");
            LOGE("    4. RTSP服务器是否运行");
        } else if (ret == AVERROR(ECONNREFUSED)) {
            LOGE("  - 连接被拒绝，请检查RTSP服务器状态");
        } else if (ret == AVERROR(ETIMEDOUT)) {
            LOGE("  - 连接超时，请检查网络连接");
        }
        
        env->ReleaseStringUTFChars(rtsp_url, url);
        return JNI_FALSE;
    }
    
    LOGI("🔗 RTSP连接建立成功，获取流信息...");
    
    // 获取流信息（使用默认设置）
    ret = avformat_find_stream_info(rtsp_input_ctx, nullptr);
    
    if (ret < 0) {
        char error_buf[256];
        av_strerror(ret, error_buf, sizeof(error_buf));
        LOGE("❌ 无法获取RTSP流信息: %s (%d)", error_buf, ret);
        avformat_close_input(&rtsp_input_ctx);
        rtsp_input_ctx = nullptr;
        env->ReleaseStringUTFChars(rtsp_url, url);
        return JNI_FALSE;
    }
    
    // 打印流信息（调试用）
    LOGI("📺 流信息 - URL: %s", url);
    LOGI("📊 流数量: %d, 时长: %s", 
         rtsp_input_ctx->nb_streams,
         (rtsp_input_ctx->duration != AV_NOPTS_VALUE) ? "有限" : "实时流");
    
    // 查找视频流
    video_stream_index = -1;
    for (unsigned int i = 0; i < rtsp_input_ctx->nb_streams; i++) {
        AVStream *stream = rtsp_input_ctx->streams[i];
        AVCodecParameters *codecpar = stream->codecpar;
        const char *codec_name = avcodec_get_name(codecpar->codec_id);
        
        if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_index == -1) {
            video_stream_index = i;
            LOGI("🎬 视频流 %d: %s (%dx%d), fps: %.2f", 
                 i, codec_name, codecpar->width, codecpar->height,
                 av_q2d(stream->avg_frame_rate));
        } else if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            LOGI("🎵 音频流 %d: %s (%d Hz, %d channels)", 
                 i, codec_name, codecpar->sample_rate, codecpar->channels);
        }
    }
    
    if (video_stream_index == -1) {
        LOGE("❌ 未找到视频流");
        avformat_close_input(&rtsp_input_ctx);
        rtsp_input_ctx = nullptr;
        env->ReleaseStringUTFChars(rtsp_url, url);
        return JNI_FALSE;
    }
    
    // 初始化超低延迟解码器
    AVStream *video_stream = rtsp_input_ctx->streams[video_stream_index];
    if (initUltraLowLatencyDecoder(video_stream) < 0) {
        LOGE("❌ 超低延迟解码器初始化失败");
        avformat_close_input(&rtsp_input_ctx);
        rtsp_input_ctx = nullptr;
        env->ReleaseStringUTFChars(rtsp_url, url);
        return JNI_FALSE;
    }
    
    // 保存视频流的时间基准，用于MediaCodec渲染时间戳计算
    video_stream_timebase = video_stream->time_base;
    LOGI("📏 视频流时间基准: %d/%d", video_stream_timebase.num, video_stream_timebase.den);
    
    // 应用激进的低延迟配置
    rtsp_input_ctx->flags |= AVFMT_FLAG_NOBUFFER;           // 禁用输入缓冲
    rtsp_input_ctx->flags |= AVFMT_FLAG_FLUSH_PACKETS;      // 立即刷新数据包
    rtsp_input_ctx->max_delay = 10000;                      // 最大延迟10ms（激进）
    
    rtsp_connected = true;
    LOGI("🎉 超低延迟RTSP流打开成功!");
    LOGI("📊 硬件解码: %s", hardware_decode_available ? "启用" : "禁用");
    
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
Java_com_jxj_CompileFfmpeg_MainActivity_startRtspRecording(JNIEnv *env, jobject /* thiz */, jstring output_path) {
#if FFMPEG_FOUND
    if (!rtsp_connected || !rtsp_input_ctx) {
        LOGE("RTSP stream not connected");
        return JNI_FALSE;
    }
    
    if (!output_path) {
        LOGE("Invalid output path");
        return JNI_FALSE;
    }

    const char *path = env->GetStringUTFChars(output_path, nullptr);
    if (!path) {
        LOGE("Cannot get output path");
        return JNI_FALSE;
    }

    LOGI("Starting RTSP recording to: %s", path);
    
    // 创建输出上下文
    int ret = avformat_alloc_output_context2(&rtsp_output_ctx, nullptr, nullptr, path);
    if (ret < 0) {
        LOGE("Failed to create output context: %d", ret);
        env->ReleaseStringUTFChars(output_path, path);
        return JNI_FALSE;
    }
    
    // 复制输入流到输出
    for (unsigned int i = 0; i < rtsp_input_ctx->nb_streams; i++) {
        AVStream *in_stream = rtsp_input_ctx->streams[i];
        AVStream *out_stream = avformat_new_stream(rtsp_output_ctx, nullptr);
        
        if (!out_stream) {
            LOGE("Failed to create output stream");
            avformat_free_context(rtsp_output_ctx);
            rtsp_output_ctx = nullptr;
            env->ReleaseStringUTFChars(output_path, path);
            return JNI_FALSE;
        }
        
        ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
        if (ret < 0) {
            LOGE("Failed to copy codec parameters: %d", ret);
            avformat_free_context(rtsp_output_ctx);
            rtsp_output_ctx = nullptr;
            env->ReleaseStringUTFChars(output_path, path);
            return JNI_FALSE;
        }
        
        out_stream->codecpar->codec_tag = 0;
    }
    
    // 打开输出文件
    if (!(rtsp_output_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&rtsp_output_ctx->pb, path, AVIO_FLAG_WRITE);
        if (ret < 0) {
            LOGE("Failed to open output file: %d", ret);
            avformat_free_context(rtsp_output_ctx);
            rtsp_output_ctx = nullptr;
            env->ReleaseStringUTFChars(output_path, path);
            return JNI_FALSE;
        }
    }
    
    // 写入头部
    ret = avformat_write_header(rtsp_output_ctx, nullptr);
    if (ret < 0) {
        LOGE("Failed to write header: %d", ret);
        if (!(rtsp_output_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&rtsp_output_ctx->pb);
        }
        avformat_free_context(rtsp_output_ctx);
        rtsp_output_ctx = nullptr;
        env->ReleaseStringUTFChars(output_path, path);
        return JNI_FALSE;
    }
    
    rtsp_recording = true;
    LOGI("✅ RTSP recording started");
    
    env->ReleaseStringUTFChars(output_path, path);
    return JNI_TRUE;
#else
    LOGE("FFmpeg not available");
    return JNI_FALSE;
#endif
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_stopRtspRecording(JNIEnv *env, jobject /* thiz */) {
#if FFMPEG_FOUND
    if (!rtsp_recording || !rtsp_output_ctx) {
        LOGE("RTSP recording not active");
        return JNI_FALSE;
    }
    
    LOGI("Stopping RTSP recording");
    
    // 写入尾部
    av_write_trailer(rtsp_output_ctx);
    
    // 关闭输出文件
    if (!(rtsp_output_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&rtsp_output_ctx->pb);
    }
    
    // 释放输出上下文
    avformat_free_context(rtsp_output_ctx);
    rtsp_output_ctx = nullptr;
    
    rtsp_recording = false;
    LOGI("✅ RTSP recording stopped");
    
    return JNI_TRUE;
#else
    LOGE("FFmpeg not available");
    return JNI_FALSE;
#endif
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_processRtspFrame(JNIEnv *env, jobject /* thiz */) {
#if FFMPEG_FOUND
    // 检查Surface重建状态（简化版本，不依赖Activity生命周期）
    if (surface_being_recreated.load()) {
        // Surface正在重建，跳过帧处理但保持连接
        static int recreating_count = 0;
        if (recreating_count++ % 100 == 0) {
            LOGD("🔄 Surface重建中，跳过帧处理 (第%d次)", recreating_count);
        }
        
        // 短暂休眠避免CPU占用过高
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return JNI_TRUE; // 返回成功但不处理帧
    }
    
    if (!rtsp_connected || !rtsp_input_ctx || !decoder_ctx) {
        return JNI_FALSE;
    }
    
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        return JNI_FALSE;
    }
    
    // 读取一帧数据 - 使用超时避免阻塞
    int ret = av_read_frame(rtsp_input_ctx, pkt);
    if (ret < 0) {
        av_packet_free(&pkt);
        if (ret == AVERROR_EOF) {
            LOGD("RTSP stream end");
        } else if (ret == AVERROR(EAGAIN)) {
            LOGD("需要更多数据，暂时跳过");
            return JNI_TRUE;
        } else {
            LOGE("Failed to read RTSP frame: %d", ret);
        }
        return JNI_FALSE;
    }
    
    // 只处理视频帧，丢弃其他类型的帧
    if (pkt->stream_index != video_stream_index) {
        av_packet_free(&pkt);
        return JNI_TRUE;
    }
    
    // 记录开始时间用于性能统计
    auto start_time = std::chrono::steady_clock::now();
    
    // 如果正在录制，先保存packet数据（在发送到解码器之前）
    AVPacket *record_pkt = nullptr;
    if (rtsp_recording && rtsp_output_ctx && pkt->stream_index < rtsp_output_ctx->nb_streams) {
        record_pkt = av_packet_alloc();
        if (record_pkt) {
            av_packet_ref(record_pkt, pkt);  // 创建packet的引用拷贝
        }
    }
            
    // 发送数据包到解码器
    ret = avcodec_send_packet(decoder_ctx, pkt);
    
    // 调试：记录packet信息
    static int packet_debug_count = 0;
    if (packet_debug_count++ % 100 == 0) {
        LOGD("📦 发送数据包: size=%d, pts=%lld, dts=%lld, flags=%d, ret=%d", 
             pkt->size, pkt->pts, pkt->dts, pkt->flags, ret);
    }
    
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN)) {
            // 解码器缓冲区满，先尝试接收帧来清空缓冲区
            static int buffer_full_count = 0;
            if (buffer_full_count++ % 10 == 0) {
                LOGD("解码器缓冲区满，先清空缓冲区 (第%d次)", buffer_full_count);
            }
            // 继续下面的帧接收逻辑来清空缓冲区
        } else if (ret == AVERROR_EOF) {
            LOGD("解码器已结束");
            av_packet_free(&pkt);
            if (record_pkt) av_packet_free(&record_pkt);
            return JNI_FALSE;
        } else {
            LOGE("发送数据包到解码器失败: %d", ret);
            av_packet_free(&pkt);
            if (record_pkt) av_packet_free(&record_pkt);
            return JNI_FALSE;
        }
    } else {
        // 成功发送packet
        static int success_count = 0;
        if (success_count++ % 100 == 0) {
            LOGD("✅ 数据包发送成功 (第%d个)", success_count);
        }
    }
    
    // 确保frame已分配
    if (!frame) {
        frame = av_frame_alloc();
        if (!frame) {
            LOGE("分配帧缓冲区失败");
            av_packet_free(&pkt);
            if (record_pkt) av_packet_free(&record_pkt);
            return JNI_FALSE;
        }
    }
    
    // 低延迟：接收解码帧，但只渲染最新的一帧
    bool has_valid_frame = false;
    int frame_count = 0;
    
    // 分配一个临时帧用于接收，避免破坏有效帧数据
    AVFrame *temp_frame = av_frame_alloc();
    if (!temp_frame) {
        LOGE("分配临时帧缓冲区失败");
        av_packet_free(&pkt);
        if (record_pkt) av_packet_free(&record_pkt);
        return JNI_FALSE;
    }
    
    // 循环接收所有可用帧，但只保留最后一帧用于渲染
    // 添加缓冲区压力检测
    static int pending_frames = 0;
    static auto last_emergency_drop = std::chrono::steady_clock::now();
    
    while (true) {
        ret = avcodec_receive_frame(decoder_ctx, temp_frame);
        
        // 调试：记录frame接收状态
        static int receive_debug_count = 0;
        if (receive_debug_count++ % 100 == 0) {
            if (ret >= 0) {
                LOGD("🎬 接收帧成功: %dx%d, format=%d, data[0]=%p", 
                     temp_frame->width, temp_frame->height, temp_frame->format, temp_frame->data[0]);
            } else {
                LOGD("🎬 接收帧状态: ret=%d", ret);
            }
        }
        
        if (ret == AVERROR(EAGAIN)) {
            // 没有更多帧可用
            static int eagain_count = 0;
            if (eagain_count++ % 50 == 0) {
                LOGD("⏸️ 解码器暂无输出帧 (EAGAIN, 第%d次)", eagain_count);
            }
            break;
        } else if (ret == AVERROR_EOF) {
            LOGD("解码器输出结束");
            av_frame_free(&temp_frame);
            av_packet_free(&pkt);
            if (record_pkt) av_packet_free(&record_pkt);
            return JNI_FALSE;
        } else if (ret < 0) {
            LOGE("从解码器接收帧失败: %d", ret);
            av_frame_free(&temp_frame);
            av_packet_free(&pkt);
            if (record_pkt) av_packet_free(&record_pkt);
            return JNI_FALSE;
        }
        
        // 成功接收到帧 - 检查缓冲区压力
        pending_frames++;
        auto now = std::chrono::steady_clock::now();
        auto time_since_drop = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_emergency_drop).count();
        
        // 紧急丢帧机制：如果缓冲区压力过大，丢弃旧帧
        if (pending_frames > 2 && time_since_drop > 66) { // 66ms = ~15fps最低保证
            static int drop_count = 0;
            if (drop_count++ % 10 == 0) {
                LOGW("🚨 紧急丢帧: 缓冲区压力过大(pending=%d)，丢弃帧以减少延迟", pending_frames);
            }
            pending_frames = 0;
            last_emergency_drop = now;
            continue; // 跳过这一帧的处理
        }
        
        // 成功接收到帧，检查是否有效
        if (temp_frame && temp_frame->width > 0 && temp_frame->height > 0 && temp_frame->format >= 0) {
            // 进一步验证帧数据
            bool frame_valid = false;
            
            // 对于硬件解码（MediaCodec），data[0]可能为空，这是正常的
            if (hardware_decode_available) {
                frame_valid = true;  // 硬件解码帧始终有效
                static int hw_log_count = 0;
                if (hw_log_count++ % 100 == 0) {
                    LOGD("🔧 硬件解码帧: %dx%d, format=%d (MediaCodec)", 
                         temp_frame->width, temp_frame->height, temp_frame->format);
                }
            } else {
                // 软件解码需要检查data指针
                frame_valid = (temp_frame->data[0] != nullptr);
                if (frame_valid) {
                    static int valid_log_count = 0;
                    if (valid_log_count++ % 100 == 0) {
                        LOGD("✅ 软件解码帧有效: data[0]=%p, size=%dx%d, format=%d, linesize=[%d,%d,%d]", 
                             temp_frame->data[0], temp_frame->width, temp_frame->height, temp_frame->format,
                             temp_frame->linesize[0], temp_frame->linesize[1], temp_frame->linesize[2]);
                    }
                } else {
                    static int invalid_log_count = 0;
                    if (invalid_log_count++ % 50 == 0) {
                        LOGW("⚠️ 软件解码帧无效: data[0]=%p, size=%dx%d, format=%d", 
                             temp_frame->data[0], temp_frame->width, temp_frame->height, temp_frame->format);
                    }
                }
            }
            
            if (frame_valid) {
                // 将有效帧数据转移到主帧缓冲区
                av_frame_unref(frame);  // 清理旧数据
                av_frame_move_ref(frame, temp_frame);  // 移动引用，避免数据拷贝
                
                has_valid_frame = true;
                frame_count++;
                pending_frames = 0; // 重置缓冲区计数
                
                // 如果有多帧，只保留最新的（跳帧降低延迟）
                if (frame_count > 1) {
                    static int skip_log_count = 0;
                    if (skip_log_count++ % 30 == 0) {
                        LOGD("⏭️ 跳过旧帧，保持最新帧 (累计跳过%d帧)", frame_count - 1);
                    }
                }
            }
        } else {
            // 记录无效帧的详细信息
            static int invalid_frame_count = 0;
            if (invalid_frame_count++ % 50 == 0) {
                LOGE("❌ 无效帧数据: data[0]=%p, size=%dx%d, format=%d", 
                     temp_frame ? temp_frame->data[0] : nullptr, 
                     temp_frame ? temp_frame->width : 0, 
                     temp_frame ? temp_frame->height : 0,
                     temp_frame ? temp_frame->format : -1);
            }
        }
    }
    
    // 清理临时帧
    av_frame_free(&temp_frame);
    
    // 只渲染最新的有效帧 - 再次检查Surface状态
    if (has_valid_frame && frame) {
        // 检查Surface是否仍然有效（简化版本）
        bool can_render = false;
        {
            std::lock_guard<std::mutex> lock(surface_mutex);
            can_render = native_window && surface_valid && surface_ready && 
                        !surface_being_recreated.load();
        }
        
        if (can_render) {
            renderFrameToSurface(frame);
            processed_frame_count++;
        } else {
            static int skip_render_count = 0;
            if (skip_render_count++ % 100 == 0) {
                LOGD("⏭️ 跳过渲染: Surface不可用 (第%d次)", skip_render_count);
            }
        }
    }
    
    // 处理录制（使用之前保存的packet拷贝）
    if (record_pkt && rtsp_output_ctx) {
        AVStream *in_stream = rtsp_input_ctx->streams[record_pkt->stream_index];
        AVStream *out_stream = rtsp_output_ctx->streams[record_pkt->stream_index];
        
        // 转换时间基
        av_packet_rescale_ts(record_pkt, in_stream->time_base, out_stream->time_base);
        record_pkt->pos = -1;
        
        ret = av_interleaved_write_frame(rtsp_output_ctx, record_pkt);
        if (ret < 0) {
            LOGE("Failed to write frame: %d", ret);
        }
        av_packet_free(&record_pkt);
    }
    
    // 记录性能统计
    auto end_time = std::chrono::steady_clock::now();
    auto decode_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    total_decode_time += decode_time;
    
    // 每100帧输出一次性能统计
    if (processed_frame_count > 0 && processed_frame_count % 100 == 0) {
        float avg_time = processed_frame_count > 0 ? (float)total_decode_time / processed_frame_count : 0.0f;
        float fps = total_decode_time > 0 ? processed_frame_count * 1000.0f / total_decode_time : 0.0f;
        LOGI("📊 性能统计: 已处理%d帧, 平均解码时间%.1fms, 处理FPS%.1f", 
             processed_frame_count, avg_time, fps);
    }
    
    // 成功接收到帧（减少日志输出频率）
    if (has_valid_frame && processed_frame_count % 60 == 0) {  // 每60帧输出一次
        LOGI("✅ 成功解码渲染一帧 (%dx%d, format=%d)", frame->width, frame->height, frame->format);
    }
    
    av_packet_free(&pkt);
    return JNI_TRUE;
#else
    return JNI_FALSE;
#endif
}

extern "C" JNIEXPORT void JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_closeRtspStream(JNIEnv *env, jobject /* thiz */) {
#if FFMPEG_FOUND
    LOGI("Closing RTSP stream");
    
    // 停止录制（如果正在录制）
    if (rtsp_recording) {
        Java_com_jxj_CompileFfmpeg_MainActivity_stopRtspRecording(env, nullptr);
    }
    
    // 关闭输入流
    if (rtsp_input_ctx) {
        avformat_close_input(&rtsp_input_ctx);
        rtsp_input_ctx = nullptr;
    }
    
    // 清理解码器
    if (decoder_ctx) {
        // 硬件设备上下文会被自动释放
        if (decoder_ctx->hw_device_ctx) {
            LOGI("🔧 释放MediaCodec硬件设备上下文");
        }
        avcodec_free_context(&decoder_ctx);
        decoder_ctx = nullptr;
    }
    
    // 重置状态
    rtsp_connected = false;
    video_stream_index = -1;
    processed_frame_count = 0;
    total_decode_time = 0;
    
    LOGI("✅ RTSP stream closed");
#else
    LOGE("FFmpeg not available");
#endif
}

// 硬件解码控制方法
extern "C" JNIEXPORT void JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_setHardwareDecodeEnabled(JNIEnv *env, jobject /* thiz */, jboolean enabled) {
    hardware_decode_enabled = enabled;
    LOGI("Hardware decode %s", enabled ? "enabled" : "disabled");
    
    // TODO: 重新初始化解码器以应用新设置
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
    std::string info = "Decoder Info:\n";
    info += "Hardware Decode Enabled: " + std::string(hardware_decode_enabled ? "Yes" : "No") + "\n";
    info += "Hardware Decode Available: " + std::string(hardware_decode_available ? "Yes" : "No") + "\n";
    info += "Current Decoder: " + std::string(hardware_decode_available && hardware_decode_enabled ? "Hardware" : "Software") + "\n";
    info += "FFmpeg Initialized: " + std::string(FFmpegManager::getInstance()->isInitialized() ? "Yes" : "No") + "\n";
    
    if (decoder_ctx) {
        info += "Decoder Context: " + std::string(decoder_ctx->codec->name) + "\n";
    }
    
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
    LOGI("Performance stats reset");
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

// 移除Activity生命周期绑定 - 改为纯Surface状态管理



extern "C" JNIEXPORT void JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_setSurface(JNIEnv *env, jobject /* thiz */, jobject surface) {
#if FFMPEG_FOUND
    // 线程安全的Surface设置
    std::lock_guard<std::mutex> lock(surface_mutex);
    
    // 防止重复设置相同的Surface
    static jobject last_surface_ref = nullptr;
    static int surface_set_count = 0;
    
    LOGI("🔄 setSurface调用 #%d: surface=%p, last_surface=%p, native_window=%p", 
         ++surface_set_count, surface, last_surface_ref, native_window);
    
    if (surface == last_surface_ref && native_window != nullptr && surface != nullptr) {
        LOGW("⚠️ Surface相同且有效，跳过重复设置 (调用#%d)", surface_set_count);
        return;
    }
    
    if (surface) {
        LOGI("🔄 Setting surface for video rendering");
        
        // 标记Surface正在重建，暂停所有渲染操作
        surface_being_recreated.store(true);
        surface_valid = false;
        surface_ready = false;
        
        // 等待任何正在进行的渲染完成
        int wait_count = 0;
        while (surface_locked && wait_count < 50) { // 增加等待时间到500ms
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            wait_count++;
        }
        
        if (surface_locked) {
            LOGW("⚠️ Surface仍被锁定，强制继续");
            surface_locked = false;
        }
        
        // 释放之前的native window
        if (native_window) {
            ANativeWindow_release(native_window);
            native_window = nullptr;
            LOGD("🗑️ 释放旧的native window");
        }
        
        // 获取新的native window
        native_window = ANativeWindow_fromSurface(env, surface);
        if (native_window) {
            // 短暂延迟确保Surface完全准备好
            std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 增加延迟到50ms
            
            surface_valid = true;
            surface_ready = true;
            surface_locked = false;
            last_surface_ref = surface;
            
            // 最后解除重建标记，允许渲染继续
            surface_being_recreated.store(false);
            
            // 通知等待的线程Surface已准备好
            surface_cv.notify_all();
            
            LOGI("✅ Native window创建成功，Surface已就绪，重建完成");
        } else {
            surface_valid = false;
            surface_ready = false;
            surface_being_recreated.store(false); // 即使失败也要解除标记
            last_surface_ref = nullptr;
            LOGE("❌ 创建native window失败");
        }
    } else {
        LOGI("🧹 清理Surface");
        
        // 标记Surface正在重建（清理阶段）
        surface_being_recreated.store(true);
        surface_valid = false;
        surface_ready = false;
        last_surface_ref = nullptr;
        
        // 等待渲染完成
        int wait_count = 0;
        while (surface_locked && wait_count < 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            wait_count++;
        }
        
        surface_locked = false;
        
        if (native_window) {
            ANativeWindow_release(native_window);
            native_window = nullptr;
            LOGD("🗑️ Surface已清理");
        }
        
        // 清理完成，解除重建标记
        surface_being_recreated.store(false);
    }
#else
    LOGE("FFmpeg not available");
#endif
}

// JNI库加载和卸载
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /* reserved */) {
    JNIEnv* env;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }

    LOGI("JNI_OnLoad: Initializing FFmpeg...");
    
    // 初始化FFmpeg
    if (!initializeFFmpegInternal()) {
        LOGE("Failed to initialize FFmpeg in JNI_OnLoad");
        // 不返回错误，允许应用继续运行，但FFmpeg功能不可用
    }

    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* /* reserved */) {
    LOGI("JNI_OnUnload: Cleaning up FFmpeg...");
    cleanupFFmpegInternal();
} 