#include <jni.h>
#include <string>
#include <android/log.h>
#include <chrono>
#include <mutex>

#define LOG_TAG "FFmpegWrapper"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

// 定义FFMPEG_FOUND为1，因为我们已编译了FFmpeg库
#ifndef FFMPEG_FOUND
#define FFMPEG_FOUND 1
#endif

#if FFMPEG_FOUND
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_mediacodec.h>
}
#endif

// 性能监控相关
static std::mutex performance_mutex;
static std::chrono::high_resolution_clock::time_point last_frame_time;
static long total_decode_time_ms = 0;
static int processed_frame_count = 0;

// 基础信息方法
extern "C" JNIEXPORT jstring JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_stringFromJNI(JNIEnv *env, jobject thiz) {
#if FFMPEG_FOUND
    std::string info = "🚀 FFmpeg Android NDK 编译成功!\n\n";
    info += "📚 支持的库:\n";
    info += "• libavformat (容器格式)\n";
    info += "• libavcodec (编解码器)\n";
    info += "• libavutil (工具库)\n";
    info += "• libswresample (音频重采样)\n\n";
    info += "🎯 优化特性:\n";
    info += "• RTSP超低延迟流媒体\n";
    info += "• 硬件解码加速 (MediaCodec)\n";
    info += "• 实时性能监控\n";
    info += "• 自动降级软件解码\n\n";
    info += "🔧 编译架构: ";
    
    #ifdef __aarch64__
        info += "arm64-v8a";
    #elif defined(__arm__)
        info += "armeabi-v7a"; 
    #elif defined(__i386__)
        info += "x86";
    #elif defined(__x86_64__)
        info += "x86_64";
    #else
        info += "unknown";
    #endif
    
    return env->NewStringUTF(info.c_str());
#else
    return env->NewStringUTF("❌ FFmpeg未编译 - 请先编译FFmpeg库");
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_getFFmpegVersion(JNIEnv *env, jobject thiz) {
#if FFMPEG_FOUND
    std::string version = "FFmpeg version: ";
    version += av_version_info();
    return env->NewStringUTF(version.c_str());
#else
    return env->NewStringUTF("FFmpeg not available - please compile FFmpeg first");
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_getVideoInfo(JNIEnv *env, jobject thiz, jstring jpath) {
#if FFMPEG_FOUND
    const char *path = env->GetStringUTFChars(jpath, nullptr);
    
    AVFormatContext *format_ctx = nullptr;
    
    // 初始化FFmpeg
    //av_register_all(); // FFmpeg 4.0+ 中不再需要
    
    // 打开视频文件
    int ret = avformat_open_input(&format_ctx, path, nullptr, nullptr);
    if (ret < 0) {
        env->ReleaseStringUTFChars(jpath, path);
        return env->NewStringUTF("无法打开视频文件");
    }
    
    // 获取流信息
    ret = avformat_find_stream_info(format_ctx, nullptr);
    if (ret < 0) {
        avformat_close_input(&format_ctx);
        env->ReleaseStringUTFChars(jpath, path);
        return env->NewStringUTF("无法获取流信息");
    }
    
    // 构建信息字符串
    std::string info = "视频信息:\n";
    info += "文件: " + std::string(path) + "\n";
    info += "时长: " + std::to_string(format_ctx->duration / AV_TIME_BASE) + " 秒\n";
    info += "比特率: " + std::to_string(format_ctx->bit_rate) + " bps\n";
    info += "流数量: " + std::to_string(format_ctx->nb_streams) + "\n";
    
    // 遍历所有流
    for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
        AVStream *stream = format_ctx->streams[i];
        AVCodecParameters *codecpar = stream->codecpar;
        
        info += "\n流 " + std::to_string(i) + ":\n";
        
        if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            info += "  类型: 视频\n";
            info += "  编解码器: " + std::string(avcodec_get_name(codecpar->codec_id)) + "\n";
            info += "  分辨率: " + std::to_string(codecpar->width) + "x" + std::to_string(codecpar->height) + "\n";
            info += "  帧率: " + std::to_string(av_q2d(stream->avg_frame_rate)) + " fps\n";
        } else if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            info += "  类型: 音频\n";
            info += "  编解码器: " + std::string(avcodec_get_name(codecpar->codec_id)) + "\n";
            info += "  采样率: " + std::to_string(codecpar->sample_rate) + " Hz\n";
            info += "  声道数: " + std::to_string(codecpar->channels) + "\n";
        }
    }
    
    // 清理资源
    avformat_close_input(&format_ctx);
    env->ReleaseStringUTFChars(jpath, path);
    
    return env->NewStringUTF(info.c_str());
#else
    return env->NewStringUTF("FFmpeg not available - please compile FFmpeg first");
#endif
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_convertVideo(JNIEnv *env, jobject thiz, 
                                                     jstring input_path, jstring output_path) {
#if FFMPEG_FOUND
    const char *input = env->GetStringUTFChars(input_path, nullptr);
    const char *output = env->GetStringUTFChars(output_path, nullptr);
    
    LOGI("开始转换视频: %s -> %s", input, output);
    
    AVFormatContext *input_ctx = nullptr;
    AVFormatContext *output_ctx = nullptr;
    AVPacket *pkt = nullptr;
    
    // 打开输入文件
    int ret = avformat_open_input(&input_ctx, input, nullptr, nullptr);
    if (ret < 0) {
        LOGE("无法打开输入文件: %s", input);
        goto cleanup;
    }
    
    // 获取输入流信息
    ret = avformat_find_stream_info(input_ctx, nullptr);
    if (ret < 0) {
        LOGE("无法获取输入流信息");
        goto cleanup;
    }
    
    // 创建输出上下文
    avformat_alloc_output_context2(&output_ctx, nullptr, nullptr, output);
    if (!output_ctx) {
        LOGE("无法创建输出上下文");
        ret = AVERROR_UNKNOWN;
        goto cleanup;
    }
    
    // 复制流
    for (unsigned int i = 0; i < input_ctx->nb_streams; i++) {
        AVStream *in_stream = input_ctx->streams[i];
        AVStream *out_stream = avformat_new_stream(output_ctx, nullptr);
        
        if (!out_stream) {
            LOGE("无法创建输出流");
            ret = AVERROR_UNKNOWN;
            goto cleanup;
        }
        
        // 复制编解码器参数
        ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
        if (ret < 0) {
            LOGE("无法复制编解码器参数");
            goto cleanup;
        }
        
        out_stream->codecpar->codec_tag = 0;
    }
    
    // 打开输出文件
    if (!(output_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&output_ctx->pb, output, AVIO_FLAG_WRITE);
        if (ret < 0) {
            LOGE("无法打开输出文件: %s", output);
            goto cleanup;
        }
    }
    
    // 写入文件头
    ret = avformat_write_header(output_ctx, nullptr);
    if (ret < 0) {
        LOGE("无法写入文件头");
        goto cleanup;
    }
    
    // 复制数据包
    pkt = av_packet_alloc();
    if (!pkt) {
        LOGE("无法分配数据包");
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }
    
    while (av_read_frame(input_ctx, pkt) >= 0) {
        AVStream *in_stream = input_ctx->streams[pkt->stream_index];
        AVStream *out_stream = output_ctx->streams[pkt->stream_index];
        
        // 转换时间基
        av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);
        pkt->pos = -1;
        
        ret = av_interleaved_write_frame(output_ctx, pkt);
        if (ret < 0) {
            LOGE("写入数据包失败");
            break;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    
    // 写入文件尾
    av_write_trailer(output_ctx);
    
    LOGI("视频转换完成");
    
cleanup:
    if (pkt) av_packet_free(&pkt);
    if (input_ctx) avformat_close_input(&input_ctx);
    if (output_ctx) {
        if (!(output_ctx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&output_ctx->pb);
        avformat_free_context(output_ctx);
    }
    
    env->ReleaseStringUTFChars(input_path, input);
    env->ReleaseStringUTFChars(output_path, output);
    
    return ret >= 0 ? JNI_TRUE : JNI_FALSE;
#else
    return JNI_FALSE;
#endif
}

// RTSP相关的全局变量
static AVFormatContext *rtsp_input_ctx = nullptr;
static AVFormatContext *rtsp_output_ctx = nullptr;
static bool rtsp_recording = false;

// 硬件解码相关变量
static AVCodecContext* hw_decoder_ctx = nullptr;
static AVCodecContext* sw_decoder_ctx = nullptr;
static enum AVHWDeviceType hw_device_type = AV_HWDEVICE_TYPE_NONE;
static AVBufferRef* hw_device_ctx = nullptr;
static bool use_hardware_decode = true;  // 默认启用硬件解码
static bool hw_decode_available = false;
static int video_stream_index = -1;

// 硬件解码器初始化函数
static int init_hw_decoder(AVCodecContext *ctx, const enum AVHWDeviceType type) {
    int err = 0;

    if ((err = av_hwdevice_ctx_create(&hw_device_ctx, type, nullptr, nullptr, 0)) < 0) {
        LOGE("无法创建硬件设备上下文 %s: %d", av_hwdevice_get_type_name(type), err);
        return err;
    }
    ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
    return err;
}

// 查找支持的硬件解码器
static enum AVHWDeviceType find_hw_device_type(const AVCodec *decoder) {
    // Android平台优先级顺序
    enum AVHWDeviceType priority_types[] = {
        AV_HWDEVICE_TYPE_MEDIACODEC,  // Android MediaCodec (最优先)
        AV_HWDEVICE_TYPE_OPENCL,      // OpenCL (通用)
        AV_HWDEVICE_TYPE_NONE
    };
    
    for (int i = 0; priority_types[i] != AV_HWDEVICE_TYPE_NONE; i++) {
        for (int j = 0;; j++) {
            const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, j);
            if (!config) {
                break;
            }
            if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
                config->device_type == priority_types[i]) {
                LOGI("找到支持的硬件解码器: %s", av_hwdevice_get_type_name(priority_types[i]));
                return priority_types[i];
            }
        }
    }
    
    LOGI("未找到支持的硬件解码器，将使用软件解码");
    return AV_HWDEVICE_TYPE_NONE;
}

// 初始化解码器（硬件优先，失败时自动降级到软件）
static int init_decoder(AVStream *stream) {
    AVCodecParameters *codecpar = stream->codecpar;
    const AVCodec *decoder = avcodec_find_decoder(codecpar->codec_id);
    
    if (!decoder) {
        LOGE("未找到解码器: %s", avcodec_get_name(codecpar->codec_id));
        return -1;
    }
    
    // 尝试硬件解码
    if (use_hardware_decode) {
        hw_device_type = find_hw_device_type(decoder);
        
        if (hw_device_type != AV_HWDEVICE_TYPE_NONE) {
            hw_decoder_ctx = avcodec_alloc_context3(decoder);
            if (!hw_decoder_ctx) {
                LOGE("无法分配硬件解码器上下文");
            } else if (avcodec_parameters_to_context(hw_decoder_ctx, codecpar) < 0) {
                LOGE("无法复制解码器参数到硬件上下文");
                avcodec_free_context(&hw_decoder_ctx);
            } else {
                // 设置硬件解码优化参数
                hw_decoder_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
                hw_decoder_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
                
                if (init_hw_decoder(hw_decoder_ctx, hw_device_type) >= 0) {
                    if (avcodec_open2(hw_decoder_ctx, decoder, nullptr) >= 0) {
                        hw_decode_available = true;
                        LOGI("✅ 硬件解码器初始化成功: %s", av_hwdevice_get_type_name(hw_device_type));
                        return 0;
                    }
                    LOGE("无法打开硬件解码器");
                    if (hw_device_ctx) {
                        av_buffer_unref(&hw_device_ctx);
                    }
                } else {
                    LOGE("硬件解码器初始化失败");
                }
                avcodec_free_context(&hw_decoder_ctx);
            }
        }
    }
    
    // 软件解码备选方案
    LOGI("初始化软件解码器...");
    sw_decoder_ctx = avcodec_alloc_context3(decoder);
    if (!sw_decoder_ctx) {
        LOGE("无法分配软件解码器上下文");
        return -1;
    }
    
    if (avcodec_parameters_to_context(sw_decoder_ctx, codecpar) < 0) {
        LOGE("无法复制解码器参数到软件上下文");
        avcodec_free_context(&sw_decoder_ctx);
        return -1;
    }
    
    // 设置软件解码超低延迟优化参数
    sw_decoder_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    sw_decoder_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    sw_decoder_ctx->thread_count = 1;
    sw_decoder_ctx->thread_type = FF_THREAD_SLICE;
    sw_decoder_ctx->max_b_frames = 0;
    sw_decoder_ctx->has_b_frames = 0;
    sw_decoder_ctx->delay = 0;
    
    if (avcodec_open2(sw_decoder_ctx, decoder, nullptr) < 0) {
        LOGE("无法打开软件解码器");
        avcodec_free_context(&sw_decoder_ctx);
        return -1;
    }
    
    hw_decode_available = false;
    LOGI("✅ 软件解码器初始化成功 (零延迟模式)");
    return 0;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_openRtspStream(JNIEnv *env, jobject thiz, jstring rtsp_url) {
#if FFMPEG_FOUND
    const char *url = env->GetStringUTFChars(rtsp_url, nullptr);
    
    LOGI("打开RTSP流: %s", url);
    
    // 清理之前的连接
    if (rtsp_input_ctx) {
        avformat_close_input(&rtsp_input_ctx);
        rtsp_input_ctx = nullptr;
    }
    
    // 创建输入上下文
    rtsp_input_ctx = avformat_alloc_context();
    
    // 设置超低延迟选项 - 基于你的优化方案
    AVDictionary *options = nullptr;
    
    // 网络层优化
    av_dict_set(&options, "rtsp_transport", "udp", 0);  // 优先UDP传输
    av_dict_set(&options, "rtsp_flags", "prefer_tcp", 0);  // TCP备用
    av_dict_set(&options, "stimeout", "3000000", 0);  // 3秒超时（减少等待）
    av_dict_set(&options, "max_delay", "50000", 0);  // 最大延迟50ms（更激进）
    
    // 缓冲区优化
    av_dict_set(&options, "fflags", "nobuffer+flush_packets", 0);  // 禁用缓冲+立即刷新
    av_dict_set(&options, "flags", "low_delay", 0);  // 低延迟标志
    av_dict_set(&options, "flags2", "fast", 0);  // 快速解码
    
    // 探测和分析优化
    av_dict_set(&options, "probesize", "32", 0);  // 最小探测大小
    av_dict_set(&options, "analyzeduration", "0", 0);  // 零分析时间
    av_dict_set(&options, "max_analyze_duration", "0", 0);  // 最大分析时间为0
    
    // 解码器缓存优化
    av_dict_set(&options, "thread_type", "slice", 0);  // 使用切片线程（更低延迟）
    av_dict_set(&options, "threads", "1", 0);  // 单线程解码（避免帧重排序）
    
    // 重排序缓冲区优化（关键优化）
    av_dict_set(&options, "reorder_queue_size", "0", 0);  // 禁用重排序队列
    av_dict_set(&options, "max_reorder_delay", "0", 0);  // 最大重排序延迟为0
    
    // 打开RTSP流
    int ret = avformat_open_input(&rtsp_input_ctx, url, nullptr, &options);
    av_dict_free(&options);
    
    if (ret < 0) {
        LOGE("无法打开RTSP流: %s, 错误码: %d", url, ret);
        env->ReleaseStringUTFChars(rtsp_url, url);
        return JNI_FALSE;
    }
    
    // 获取流信息
    ret = avformat_find_stream_info(rtsp_input_ctx, nullptr);
    if (ret < 0) {
        LOGE("无法获取RTSP流信息: %d", ret);
        avformat_close_input(&rtsp_input_ctx);
        rtsp_input_ctx = nullptr;
        env->ReleaseStringUTFChars(rtsp_url, url);
        return JNI_FALSE;
    }
    
    // 查找视频流并初始化硬件解码器
    video_stream_index = -1;
    for (unsigned int i = 0; i < rtsp_input_ctx->nb_streams; i++) {
        AVStream *stream = rtsp_input_ctx->streams[i];
        AVCodecParameters *codecpar = stream->codecpar;
        
        if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_index == -1) {
            video_stream_index = i;
            
            // 初始化解码器（硬件优先，自动降级到软件）
            if (init_decoder(stream) < 0) {
                LOGE("解码器初始化失败");
                avformat_close_input(&rtsp_input_ctx);
                rtsp_input_ctx = nullptr;
                env->ReleaseStringUTFChars(rtsp_url, url);
                return JNI_FALSE;
            }
            
            // 应用流级别的零延迟配置 (delay属性在解码器上下文中设置，不在codecpar中)
            break;
        }
    }
    
    if (video_stream_index == -1) {
        LOGE("未找到视频流");
        avformat_close_input(&rtsp_input_ctx);
        rtsp_input_ctx = nullptr;
        env->ReleaseStringUTFChars(rtsp_url, url);
        return JNI_FALSE;
    }
    
    LOGI("RTSP流打开成功, 流数量: %d", rtsp_input_ctx->nb_streams);
    
    // 打印流信息
    for (unsigned int i = 0; i < rtsp_input_ctx->nb_streams; i++) {
        AVStream *stream = rtsp_input_ctx->streams[i];
        AVCodecParameters *codecpar = stream->codecpar;
        
        if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            LOGI("视频流 %d: %s, %dx%d, fps: %.2f", 
                i, avcodec_get_name(codecpar->codec_id),
                codecpar->width, codecpar->height,
                av_q2d(stream->avg_frame_rate));
        } else if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            LOGI("音频流 %d: %s, %d Hz, %d channels", 
                i, avcodec_get_name(codecpar->codec_id),
                codecpar->sample_rate, codecpar->channels);
        }
    }
    
    env->ReleaseStringUTFChars(rtsp_url, url);
    return JNI_TRUE;
#else
    return JNI_FALSE;
#endif
}

// 硬件解码开关控制
extern "C" JNIEXPORT void JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_setHardwareDecodeEnabled(JNIEnv *env, jobject thiz, jboolean enabled) {
#if FFMPEG_FOUND
    use_hardware_decode = enabled;
    LOGI("硬件解码已%s", enabled ? "启用" : "禁用");
#endif
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_isHardwareDecodeEnabled(JNIEnv *env, jobject thiz) {
#if FFMPEG_FOUND
    return use_hardware_decode ? JNI_TRUE : JNI_FALSE;
#else
    return JNI_FALSE;
#endif
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_isHardwareDecodeAvailable(JNIEnv *env, jobject thiz) {
#if FFMPEG_FOUND
    return hw_decode_available ? JNI_TRUE : JNI_FALSE;
#else
    return JNI_FALSE;
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_getDecoderInfo(JNIEnv *env, jobject thiz) {
#if FFMPEG_FOUND
    std::string info = "解码器状态:\n";
    info += "硬件解码开关: " + std::string(use_hardware_decode ? "启用" : "禁用") + "\n";
    info += "当前解码器: ";
    
    if (hw_decode_available && hw_decoder_ctx) {
        info += "硬件解码 (" + std::string(av_hwdevice_get_type_name(hw_device_type)) + ")\n";
        info += "解码器: " + std::string(hw_decoder_ctx->codec->name) + "\n";
    } else if (sw_decoder_ctx) {
        info += "软件解码\n";
        info += "解码器: " + std::string(sw_decoder_ctx->codec->name) + "\n";
        info += "零延迟模式: 启用\n";
    } else {
        info += "未初始化\n";
    }
    
    // 列出支持的硬件解码类型
    info += "\n支持的硬件解码类型:\n";
    enum AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
    while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE) {
        info += "  - " + std::string(av_hwdevice_get_type_name(type)) + "\n";
    }
    
    return env->NewStringUTF(info.c_str());
#else
    return env->NewStringUTF("FFmpeg not available");
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_getRtspStreamInfo(JNIEnv *env, jobject thiz) {
#if FFMPEG_FOUND
    if (!rtsp_input_ctx) {
        return env->NewStringUTF("RTSP流未打开");
    }
    
    std::string info = "RTSP流信息:\n";
    info += "URL: " + std::string(rtsp_input_ctx->url ? rtsp_input_ctx->url : "unknown") + "\n";
    info += "时长: " + (rtsp_input_ctx->duration != AV_NOPTS_VALUE ? 
                      std::to_string(rtsp_input_ctx->duration / AV_TIME_BASE) + " 秒" : "实时流") + "\n";
    info += "比特率: " + std::to_string(rtsp_input_ctx->bit_rate) + " bps\n";
    info += "流数量: " + std::to_string(rtsp_input_ctx->nb_streams) + "\n";
    
    for (unsigned int i = 0; i < rtsp_input_ctx->nb_streams; i++) {
        AVStream *stream = rtsp_input_ctx->streams[i];
        AVCodecParameters *codecpar = stream->codecpar;
        
        info += "\n流 " + std::to_string(i) + ":\n";
        
        if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            info += "  类型: 视频\n";
            info += "  编解码器: " + std::string(avcodec_get_name(codecpar->codec_id)) + "\n";
            info += "  分辨率: " + std::to_string(codecpar->width) + "x" + std::to_string(codecpar->height) + "\n";
            info += "  帧率: " + std::to_string(av_q2d(stream->avg_frame_rate)) + " fps\n";
        } else if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            info += "  类型: 音频\n";
            info += "  编解码器: " + std::string(avcodec_get_name(codecpar->codec_id)) + "\n";
            info += "  采样率: " + std::to_string(codecpar->sample_rate) + " Hz\n";
            info += "  声道数: " + std::to_string(codecpar->channels) + "\n";
        }
    }
    
    return env->NewStringUTF(info.c_str());
#else
    return env->NewStringUTF("FFmpeg not available");
#endif
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_startRtspRecording(JNIEnv *env, jobject thiz, jstring output_path) {
#if FFMPEG_FOUND
    if (!rtsp_input_ctx) {
        LOGE("RTSP流未打开，无法开始录制");
        return JNI_FALSE;
    }
    
    if (rtsp_recording) {
        LOGE("录制已在进行中");
        return JNI_FALSE;
    }
    
    const char *output = env->GetStringUTFChars(output_path, nullptr);
    
    LOGI("开始录制RTSP流到: %s", output);
    
    // 创建输出上下文
    int ret = avformat_alloc_output_context2(&rtsp_output_ctx, nullptr, nullptr, output);
    if (ret < 0) {
        LOGE("无法创建输出上下文: %d", ret);
        env->ReleaseStringUTFChars(output_path, output);
        return JNI_FALSE;
    }
    
    // 复制流到输出
    for (unsigned int i = 0; i < rtsp_input_ctx->nb_streams; i++) {
        AVStream *in_stream = rtsp_input_ctx->streams[i];
        AVStream *out_stream = avformat_new_stream(rtsp_output_ctx, nullptr);
        
        if (!out_stream) {
            LOGE("无法创建输出流");
            avformat_free_context(rtsp_output_ctx);
            rtsp_output_ctx = nullptr;
            env->ReleaseStringUTFChars(output_path, output);
            return JNI_FALSE;
        }
        
        // 复制编解码器参数
        ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
        if (ret < 0) {
            LOGE("无法复制编解码器参数: %d", ret);
            avformat_free_context(rtsp_output_ctx);
            rtsp_output_ctx = nullptr;
            env->ReleaseStringUTFChars(output_path, output);
            return JNI_FALSE;
        }
        
        out_stream->codecpar->codec_tag = 0;
        
        // 设置时间基 - 注意：flags在codecpar中不存在，此设置对于流拷贝实际上不是必需的
    }
    
    // 打开输出文件
    if (!(rtsp_output_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&rtsp_output_ctx->pb, output, AVIO_FLAG_WRITE);
        if (ret < 0) {
            LOGE("无法打开输出文件: %s, 错误码: %d", output, ret);
            avformat_free_context(rtsp_output_ctx);
            rtsp_output_ctx = nullptr;
            env->ReleaseStringUTFChars(output_path, output);
            return JNI_FALSE;
        }
    }
    
    // 写入文件头
    ret = avformat_write_header(rtsp_output_ctx, nullptr);
    if (ret < 0) {
        LOGE("无法写入输出文件头: %d", ret);
        if (!(rtsp_output_ctx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&rtsp_output_ctx->pb);
        avformat_free_context(rtsp_output_ctx);
        rtsp_output_ctx = nullptr;
        env->ReleaseStringUTFChars(output_path, output);
        return JNI_FALSE;
    }
    
    rtsp_recording = true;
    LOGI("RTSP录制开始成功");
    
    env->ReleaseStringUTFChars(output_path, output);
    return JNI_TRUE;
#else
    return JNI_FALSE;
#endif
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_stopRtspRecording(JNIEnv *env, jobject thiz) {
#if FFMPEG_FOUND
    if (!rtsp_recording || !rtsp_output_ctx) {
        LOGE("录制未在进行中");
        return JNI_FALSE;
    }
    
    LOGI("停止RTSP录制");
    
    // 写入文件尾
    av_write_trailer(rtsp_output_ctx);
    
    // 关闭输出文件
    if (!(rtsp_output_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&rtsp_output_ctx->pb);
    
    // 释放输出上下文
    avformat_free_context(rtsp_output_ctx);
    rtsp_output_ctx = nullptr;
    rtsp_recording = false;
    
    LOGI("RTSP录制停止成功");
    return JNI_TRUE;
#else
    return JNI_FALSE;
#endif
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_processRtspFrame(JNIEnv *env, jobject thiz) {
#if FFMPEG_FOUND
    if (!rtsp_input_ctx) {
        return JNI_FALSE;
    }
    
    // 开始性能计时
    auto frame_start_time = std::chrono::high_resolution_clock::now();
    
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        return JNI_FALSE;
    }
    
    // 读取一帧数据
    int ret = av_read_frame(rtsp_input_ctx, pkt);
    if (ret < 0) {
        if (ret == AVERROR_EOF) {
            LOGD("RTSP流结束");
        } else {
            LOGE("读取RTSP帧失败: %d", ret);
        }
        av_packet_free(&pkt);
        return JNI_FALSE;
    }
    
    // 解码性能监控（仅对视频帧）
    bool is_video_frame = false;
    if (pkt->stream_index == video_stream_index) {
        is_video_frame = true;
        
        // 如果有解码器，进行解码测试
        AVCodecContext *decoder = hw_decode_available ? hw_decoder_ctx : sw_decoder_ctx;
        if (decoder) {
            auto decode_start = std::chrono::high_resolution_clock::now();
            
            // 发送数据包到解码器
            ret = avcodec_send_packet(decoder, pkt);
            if (ret == 0) {
                AVFrame *frame = av_frame_alloc();
                if (frame) {
                    // 接收解码后的帧
                    ret = avcodec_receive_frame(decoder, frame);
                    if (ret == 0) {
                        // 解码成功，计算解码时间
                        auto decode_end = std::chrono::high_resolution_clock::now();
                        auto decode_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                            decode_end - decode_start).count();
                        
                        // 更新性能统计
                        std::lock_guard<std::mutex> lock(performance_mutex);
                        total_decode_time_ms += decode_duration;
                        processed_frame_count++;
                        
                        LOGD("解码帧: %d, 耗时: %ld ms, 平均: %.2f ms", 
                             processed_frame_count, decode_duration, 
                             (float)total_decode_time_ms / processed_frame_count);
                    }
                    av_frame_free(&frame);
                }
            }
        }
    }
    
    // 如果正在录制，写入输出文件
    if (rtsp_recording && rtsp_output_ctx) {
        AVStream *in_stream = rtsp_input_ctx->streams[pkt->stream_index];
        AVStream *out_stream = rtsp_output_ctx->streams[pkt->stream_index];
        
        // 转换时间基
        av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);
        pkt->pos = -1;
        
        ret = av_interleaved_write_frame(rtsp_output_ctx, pkt);
        if (ret < 0) {
            LOGE("写入录制帧失败: %d", ret);
        }
    }
    
    av_packet_unref(pkt);
    av_packet_free(&pkt);
    
    // 计算总帧处理时间
    auto frame_end_time = std::chrono::high_resolution_clock::now();
    auto frame_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        frame_end_time - frame_start_time).count();
    
    if (is_video_frame && frame_duration > 50) {
        LOGD("⚠️ 帧处理时间较长: %ld ms", frame_duration);
    }
    
    return JNI_TRUE;
#else
    return JNI_FALSE;
#endif
}

extern "C" JNIEXPORT void JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_closeRtspStream(JNIEnv *env, jobject thiz) {
#if FFMPEG_FOUND
    LOGI("关闭RTSP流");
    
    // 停止录制
    if (rtsp_recording) {
        Java_com_jxj_CompileFfmpeg_MainActivity_stopRtspRecording(env, thiz);
    }
    
    // 清理硬件解码器资源
    if (hw_decoder_ctx) {
        avcodec_free_context(&hw_decoder_ctx);
        hw_decoder_ctx = nullptr;
        LOGI("硬件解码器已清理");
    }
    
    if (sw_decoder_ctx) {
        avcodec_free_context(&sw_decoder_ctx);
        sw_decoder_ctx = nullptr;
        LOGI("软件解码器已清理");
    }
    
    if (hw_device_ctx) {
        av_buffer_unref(&hw_device_ctx);
        hw_device_ctx = nullptr;
        LOGI("硬件设备上下文已清理");
    }
    
    // 重置状态
    hw_decode_available = false;
    hw_device_type = AV_HWDEVICE_TYPE_NONE;
    video_stream_index = -1;
    
    // 关闭输入流
    if (rtsp_input_ctx) {
        avformat_close_input(&rtsp_input_ctx);
        rtsp_input_ctx = nullptr;
    }
    
    LOGI("RTSP流关闭完成");
#endif
}

// 新增性能监控方法
extern "C" JNIEXPORT jstring JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_getPerformanceStats(JNIEnv *env, jobject thiz) {
#if FFMPEG_FOUND
    std::lock_guard<std::mutex> lock(performance_mutex);
    
    std::string stats = "📊 性能统计:\n";
    stats += "处理帧数: " + std::to_string(processed_frame_count) + "\n";
    
    if (processed_frame_count > 0) {
        float avg_decode_time = (float)total_decode_time_ms / processed_frame_count;
        stats += "平均解码时间: " + std::to_string(avg_decode_time) + " ms\n";
        stats += "总解码时间: " + std::to_string(total_decode_time_ms) + " ms\n";
        
        // 计算估算的FPS（基于解码时间）
        if (avg_decode_time > 0) {
            float estimated_fps = 1000.0f / avg_decode_time;
            stats += "理论最大FPS: " + std::to_string(estimated_fps) + "\n";
        }
    } else {
        stats += "暂无性能数据\n";
    }
    
    // 添加解码器状态
    stats += "\n🔧 解码器状态:\n";
    if (hw_decode_available && hw_decoder_ctx) {
        stats += "硬件解码: ✅ 启用\n";
        stats += "硬件类型: " + std::string(av_hwdevice_get_type_name(hw_device_type)) + "\n";
    } else if (sw_decoder_ctx) {
        stats += "软件解码: ✅ 启用\n";
        stats += "硬件解码: ❌ 不可用\n";
    } else {
        stats += "解码器: ❌ 未初始化\n";
    }
    
    return env->NewStringUTF(stats.c_str());
#else
    return env->NewStringUTF("FFmpeg未可用");
#endif
}

extern "C" JNIEXPORT void JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_resetPerformanceStats(JNIEnv *env, jobject thiz) {
#if FFMPEG_FOUND
    std::lock_guard<std::mutex> lock(performance_mutex);
    total_decode_time_ms = 0;
    processed_frame_count = 0;
    LOGI("性能统计已重置");
#endif
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_getAverageDecodeTime(JNIEnv *env, jobject thiz) {
#if FFMPEG_FOUND
    std::lock_guard<std::mutex> lock(performance_mutex);
    if (processed_frame_count > 0) {
        return total_decode_time_ms / processed_frame_count;
    }
    return 0;
#else
    return 0;
#endif
}

extern "C" JNIEXPORT jint JNICALL
Java_com_jxj_CompileFfmpeg_MainActivity_getProcessedFrameCount(JNIEnv *env, jobject thiz) {
#if FFMPEG_FOUND
    std::lock_guard<std::mutex> lock(performance_mutex);
    return processed_frame_count;
#else
    return 0;
#endif
} 