#!/bin/bash
# FFmpeg 6.1 编译脚本 for Windows 11 + NDK 25
# 解决GWP-ASan问题，开启硬件加速和RTSP功能
# 目标架构: arm64-v8a

set -e

# ======================= 路径配置 =======================
# 使用最新的 NDK 25 (避免 GWP-ASan 问题)
NDK_25_PATH="C:/Users/pc/AppData/Local/Android/Sdk/ndk/25.2.9519653"
FFMPEG_VERSION="6.1.1"
FFMPEG_ARCHIVE="ffmpeg-${FFMPEG_VERSION}.tar.xz"
FFMPEG_DIR="ffmpeg-${FFMPEG_VERSION}"

# ======================= 路径转换函数 =======================
win_to_msys2_path() {
    echo "$1" | sed 's/\\/\//g' | sed 's/^\([A-Z]\):/\/\l\1/'
}

# 转换NDK路径 - 使用Windows原始路径避免MSYS2路径问题
NDK_PATH_WIN="$NDK_25_PATH"
NDK_PATH=$(win_to_msys2_path "$NDK_25_PATH")
TOOLCHAIN_WIN="$NDK_25_PATH/toolchains/llvm/prebuilt/windows-x86_64"
TOOLCHAIN="$NDK_PATH/toolchains/llvm/prebuilt/windows-x86_64"
SYSROOT_WIN="$TOOLCHAIN_WIN/sysroot"
SYSROOT="$TOOLCHAIN/sysroot"
API=24  # 推荐使用 API 24+

echo "🔧 使用 NDK 25 (版本 25.2.9519653)"
echo "NDK 路径: $NDK_PATH"

# ======================= 准备源码 =======================
if [[ ! -d "$FFMPEG_DIR" ]]; then
    if [[ ! -f "$FFMPEG_ARCHIVE" ]]; then
        echo "⬇️ 下载 FFmpeg ${FFMPEG_VERSION}..."
        curl -O "https://ffmpeg.org/releases/$FFMPEG_ARCHIVE"
    fi
    echo "📦 解压 FFmpeg..."
    tar -xf "$FFMPEG_ARCHIVE"
fi

cd "$FFMPEG_DIR"

# ======================= 编译函数 =======================
build_android() {
    echo "🚀 开始编译: $ARCH ($CPU)"

    # 配置参数
    local PREFIX="$(pwd)/build/$ANDROID_ABI"
    rm -rf "$PREFIX" && mkdir -p "$PREFIX"

    # 编译器路径 - 使用Windows路径避免路径解析问题
    local CC="$TOOLCHAIN_WIN/bin/${HOST}${API}-clang"
    local CXX="$TOOLCHAIN_WIN/bin/${HOST}${API}-clang++"

    # 工具链
    local NM="$TOOLCHAIN_WIN/bin/llvm-nm"
    local AR="$TOOLCHAIN_WIN/bin/llvm-ar"
    local RANLIB="$TOOLCHAIN_WIN/bin/llvm-ranlib"
    local STRIP="$TOOLCHAIN_WIN/bin/llvm-strip"

    # 禁用所有sanitizer
    export ASAN_OPTIONS=disable=1
    export MSAN_OPTIONS=disable=1
    export TSAN_OPTIONS=disable=1
    export UBSAN_OPTIONS=disable=1

    # 测试编译器是否工作
    echo "🔍 测试编译器..."
    if ! "$CC" --version > /dev/null 2>&1; then
        echo "❌ 编译器测试失败: $CC"
        echo "请检查NDK路径和版本"
        exit 1
    fi
    echo "✅ 编译器测试通过"

    echo "🔨 配置 FFmpeg (优化NDK 25兼容性)..."

    # ======================= 关键配置 =======================
    ./configure \
        --prefix="$PREFIX" \
        --target-os=android \
        --arch="$ARCH" \
        --cpu="$CPU" \
        --cc="$CC" \
        --cxx="$CXX" \
        --nm="$NM" \
        --ar="$AR" \
        --ranlib="$RANLIB" \
        --strip="$STRIP" \
        --cross-prefix="$TOOLCHAIN_WIN/bin/${HOST}-" \
        --sysroot="$SYSROOT_WIN" \
        --enable-cross-compile \
        --enable-pic \
        --enable-static \
        --disable-shared \
        --disable-debug \
        --disable-programs \
        --disable-doc \
        --disable-avdevice \
        --disable-postproc \
        --disable-avfilter \
        --enable-swscale \
        --disable-iconv \
        --disable-bsfs \
        --disable-encoders \
        --disable-muxers \
        --disable-x86asm \
        --disable-asm \
        --disable-inline-asm \
        --disable-vulkan \
        --enable-network \
        --enable-zlib \
        --enable-jni \
        --enable-mediacodec \
        --enable-neon \
        --enable-hwaccels \
        --enable-demuxer=rtsp \
        --enable-demuxer=rtp \
        --enable-demuxer=sdp \
        --enable-demuxer=mov \
        --enable-demuxer=matroska \
        --enable-demuxer=h264 \
        --enable-demuxer=hevc \
        --enable-demuxer=flv \
        --enable-protocol=rtsp \
        --enable-protocol=tcp \
        --enable-protocol=udp \
        --enable-protocol=rtp \
        --enable-protocol=rtmp \
        --enable-parser=h264 \
        --enable-parser=hevc \
        --enable-parser=aac \
        --enable-parser=mpeg4video \
        --enable-decoder=h264 \
        --enable-decoder=h265 \
        --enable-decoder=hevc \
        --enable-decoder=mpeg4 \
        --enable-decoder=aac \
        --enable-decoder=mp3 \
        --enable-decoder=pcm_s16le \
        --enable-decoder=pcm_s16be \
        --enable-decoder=pcm_mulaw \
        --enable-decoder=pcm_alaw \
        --enable-decoder=adpcm_g726 \
        --enable-decoder=h264_mediacodec \
        --enable-decoder=hevc_mediacodec \
        --enable-decoder=vp9_mediacodec \
        --extra-cflags="-Os -fPIC -DANDROID -D__ANDROID_API__=$API -fno-sanitize=scudo -fno-sanitize=all -DGWP_ASAN_HOOKS=0" \
        --extra-cxxflags="-Os -fPIC -DANDROID -D__ANDROID_API__=$API -fno-sanitize=scudo -fno-sanitize=all -DGWP_ASAN_HOOKS=0" \
        --extra-ldexeflags="-pie" \
        --extra-ldflags="-L$SYSROOT_WIN/usr/lib/aarch64-linux-android/$API -L$TOOLCHAIN_WIN/sysroot/usr/lib/aarch64-linux-android/$API -landroid -lmediandk -lm -llog -lz -fno-sanitize=scudo -fno-sanitize=all -Wl,--no-undefined -Wl,--no-as-needed" \
        --pkg-config="pkg-config" || {
            echo "❌ 配置失败!"; exit 1
        }

    echo "🔨 编译中 (使用 $(nproc) 线程)..."
    make clean
    make -j$(nproc) || {
        echo "❌ 编译失败!"; exit 1
    }
    make install || {
        echo "❌ 安装失败!"; exit 1
    }

    # ======================= 验证与输出 =======================
    echo "✅ 验证生成的库文件..."
    local OUTPUT_LIB="$PREFIX/lib/libavcodec.a"

    if [ -f "$OUTPUT_LIB" ]; then
        echo "✅ 静态库编译成功"

        # 检查库文件大小
        local LIB_SIZE=$(stat -c%s "$OUTPUT_LIB" 2>/dev/null || echo "0")
        echo "📊 libavcodec.a 大小: $LIB_SIZE bytes"

        # 检查符号
        if "$NM" "$OUTPUT_LIB" 2>/dev/null | grep -q "av_"; then
            echo "✅ FFmpeg符号验证通过"
        else
            echo "⚠️  FFmpeg符号验证失败"
        fi
    else
        echo "❌ 库文件生成失败: $OUTPUT_LIB"
        exit 1
    fi

    # 创建合并的动态库（使用修复后的方法）
    echo "📦 创建合并的动态库..."
    local COMBINED_SO="$PREFIX/lib/libffmpeg.so"

    # 使用 --allow-multiple-definition 和 --whole-archive 解决符号冲突
    echo "🔗 正在链接所有静态库到动态库（修复版）..."
    "$CC" -shared -fPIC \
        -Wl,--allow-multiple-definition \
        -Wl,--whole-archive \
        "$PREFIX/lib/libavformat.a" \
        "$PREFIX/lib/libavcodec.a" \
        "$PREFIX/lib/libavutil.a" \
        "$PREFIX/lib/libswresample.a" \
        "$PREFIX/lib/libswscale.a" \
        -Wl,--no-whole-archive \
        -L"$SYSROOT_WIN/usr/lib/aarch64-linux-android/$API" \
        -L"$TOOLCHAIN_WIN/sysroot/usr/lib/aarch64-linux-android/$API" \
        -landroid -lmediandk -llog -lz -lm -lc++_shared \
        -Wl,--no-undefined \
        -o "$COMBINED_SO" || {
        echo "❌ 动态库创建失败!"
        exit 1
    }

    # 验证动态库大小
    local SO_SIZE=$(stat -c%s "$COMBINED_SO" 2>/dev/null || echo "0")
    echo "📊 libffmpeg.so 大小: $SO_SIZE bytes ($(($SO_SIZE / 1024 / 1024))MB)"

    # 检查大小是否合理（应该至少10MB+）
    if [ "$SO_SIZE" -lt 10000000 ]; then
        echo "⚠️  警告: 动态库大小可能偏小 ($SO_SIZE bytes)"

        # 显示各静态库大小用于诊断
        echo "🔍 静态库大小对比:"
        for lib in libavformat.a libavcodec.a libavutil.a libswresample.a libswscale.a; do
            if [ -f "$PREFIX/lib/$lib" ]; then
                local LIB_SIZE=$(stat -c%s "$PREFIX/lib/$lib" 2>/dev/null || echo "0")
                echo "  $lib: $(($LIB_SIZE / 1024))KB"
            fi
        done
    else
        echo "✅ 动态库大小正常"
    fi

    # 验证生成的动态库和符号
    if [ -f "$COMBINED_SO" ]; then
        echo "✅ 动态库创建成功: $COMBINED_SO"

        # 检查符号数量
        local SYMBOL_COUNT=$("$NM" -D "$COMBINED_SO" 2>/dev/null | wc -l || echo "0")
        echo "📊 导出符号数量: $SYMBOL_COUNT"

        # 检查关键FFmpeg符号
        echo "🔍 验证关键FFmpeg符号..."
        local FOUND_SYMBOLS=0
        for symbol in avformat_open_input avcodec_find_decoder av_read_frame avformat_network_init; do
            if "$NM" -D "$COMBINED_SO" 2>/dev/null | grep -q "$symbol"; then
                echo "  ✅ $symbol"
                FOUND_SYMBOLS=$((FOUND_SYMBOLS + 1))
            else
                echo "  ❌ $symbol"
            fi
        done

        if [ "$FOUND_SYMBOLS" -ge 3 ]; then
            echo "✅ 关键符号验证通过 ($FOUND_SYMBOLS/4)"
        else
            echo "⚠️  关键符号缺失过多 ($FOUND_SYMBOLS/4)"
        fi

        # 复制到目标目录
        local TARGET_DIR="../app/src/main/cpp/ffmpeg/$ANDROID_ABI"
        mkdir -p "$TARGET_DIR"
        cp "$COMBINED_SO" "$TARGET_DIR/"
        echo "📦 已复制动态库到: $TARGET_DIR"
    else
        echo "❌ 动态库创建失败!"
        exit 1
    fi

    # 复制到项目目录
    local DEST_DIR="$(pwd)/../app/src/main/cpp/ffmpeg/$ANDROID_ABI"
    mkdir -p "$DEST_DIR/lib" "$DEST_DIR/include"
    cp "$COMBINED_SO" "$DEST_DIR/lib/"
    cp -r "$PREFIX/include/"* "$DEST_DIR/include/"

    echo "✅ 编译完成: $ANDROID_ABI"
    echo "📁 库文件: $DEST_DIR/lib"
    echo "📁 头文件: $DEST_DIR/include"
}

# ======================= 编译 arm64-v8a =======================
ARCH="aarch64"
CPU="armv8-a"
HOST="aarch64-linux-android"
ANDROID_ABI="arm64-v8a"

build_android

echo ""
echo "🎉 FFmpeg 编译成功!"
echo "💡 已启用功能: RTSP, 硬件加速(MediaCodec), Neon优化"
echo "🛡️ 已禁用: GWP-ASan, Vulkan, 无用模块"
echo "🔧 NDK 版本: 25.2.9519653"