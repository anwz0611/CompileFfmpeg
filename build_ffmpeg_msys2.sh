#!/bin/bash
# FFmpeg Android NDK 编译脚本 - 基于成功脚本重写
# NDK 26.1.10909125

set -e

# Convert Windows path to MSYS2 path format
win_to_msys2_path() {
    echo "$1" | sed 's/\\/\//g' | sed 's/C:/\/c/g' | sed 's/D:/\/d/g'
}

# NDK路径，使用 Windows 路径格式 - 使用NDK 24
ANDROID_NDK_ROOT_WIN="C:/Users/pc/AppData/Local/Android/Sdk/ndk/23.2.8568313"
ANDROID_NDK_ROOT=$(win_to_msys2_path "$ANDROID_NDK_ROOT_WIN")
TOOLCHAIN=$(win_to_msys2_path "$ANDROID_NDK_ROOT_WIN/toolchains/llvm/prebuilt/windows-x86_64")
API=24

function check_toolchain() {
    echo "Checking NDK tools..."

    if [ ! -d "$ANDROID_NDK_ROOT" ]; then
        echo "Error: NDK directory not found at $ANDROID_NDK_ROOT"
        exit 1
    fi

    if [ ! -d "$TOOLCHAIN" ]; then
        echo "Error: Toolchain directory not found at $TOOLCHAIN"
        exit 1
    fi

    # Remove .cmd extension for MSYS2 environment
    CC_PATH=$(echo "$CC" | sed 's/\.cmd$//')
    CXX_PATH=$(echo "$CXX" | sed 's/\.cmd$//')

    if [ ! -f "$CC_PATH" ]; then
        echo "Error: C compiler not found at $CC_PATH"
        exit 1
    fi

    if [ ! -f "$CXX_PATH" ]; then
        echo "Error: C++ compiler not found at $CXX_PATH"
        exit 1
    fi

    echo "NDK directory: $ANDROID_NDK_ROOT"
    echo "Toolchain directory: $TOOLCHAIN"
    echo "C compiler: $CC"
    echo "C++ compiler: $CXX"
}

# 准备FFmpeg源码
FFMPEG_DIR="ffmpeg-6.1.1"
if [[ ! -d "$FFMPEG_DIR" ]]; then
    if [[ ! -f "ffmpeg-6.1.1.tar.xz" ]]; then
        wget "https://ffmpeg.org/releases/ffmpeg-6.1.1.tar.xz"
    fi
    tar -xf "ffmpeg-6.1.1.tar.xz"
fi

cd "$FFMPEG_DIR"

function build_android() {
    echo "开始编译 $CPU"

    # 检查工具链
    check_toolchain

    # 确保目录存在
    mkdir -p "$PREFIX"

    # 设置pkg-config
    export PKG_CONFIG_PATH="$ANDROID_NDK_ROOT_WIN/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/pkgconfig"
    export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"

    # 直接使用Windows路径，不转换 - 参考成功脚本
    CC_CONFIGURE="$ANDROID_NDK_ROOT_WIN/toolchains/llvm/prebuilt/windows-x86_64/bin/$HOST$API-clang"
    CXX_CONFIGURE="$ANDROID_NDK_ROOT_WIN/toolchains/llvm/prebuilt/windows-x86_64/bin/$HOST$API-clang++"
    NM="$ANDROID_NDK_ROOT_WIN/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-nm"
    AR="$ANDROID_NDK_ROOT_WIN/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-ar"
    RANLIB="$ANDROID_NDK_ROOT_WIN/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-ranlib"
    STRIP="$ANDROID_NDK_ROOT_WIN/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip"

    # 配置FFmpeg - 修改编译选项
    ./configure \
        --prefix="$PREFIX" \
        --target-os=android \
        --arch="$ARCH" \
        --cpu="$CPU" \
        --cc="$CC_CONFIGURE" \
        --cxx="$CXX_CONFIGURE" \
        --nm="$NM" \
        --ar="$AR" \
        --ranlib="$RANLIB" \
        --strip="$STRIP" \
        --cross-prefix="$HOST$API-" \
        --sysroot="$ANDROID_NDK_ROOT_WIN/toolchains/llvm/prebuilt/windows-x86_64/sysroot" \
        --enable-cross-compile \
        --disable-debug \
        --disable-programs \
        --disable-doc \
        --disable-avdevice \
        --disable-avfilter \
        --disable-postproc \
        --enable-swscale \
        --enable-pic \
        --enable-static \
        --disable-shared \
        --enable-small \
        --enable-zlib \
        --pkg-config-flags="--static" \
        --enable-network \
        --disable-x86asm \
        --disable-asm \
        --disable-inline-asm \
        --disable-iconv \
        --disable-securetransport \
        --disable-xlib \
        --disable-devices \
        --disable-outdevs \
        --disable-indevs \
        --disable-filters \
        --disable-bsfs \
        --disable-parsers \
        --enable-parser=h264 \
        --enable-parser=aac \
        --disable-encoders \
        --enable-encoder=aac \
        --disable-decoders \
        --enable-decoder=h264 \
        --enable-decoder=aac \
        --enable-decoder=pcm_s16le \
        --enable-decoder=h264_mediacodec \
        --enable-hwaccel=h264_mediacodec_async \
        --enable-mediacodec \
        --enable-jni \
        --disable-muxers \
        --enable-muxer=mp4 \
        --enable-muxer=mov \
        --disable-demuxers \
        --enable-demuxer=mov \
        --enable-demuxer=matroska \
        --enable-demuxer=rtsp \
        --enable-demuxer=sdp \
        --enable-demuxer=rtp \
        --disable-protocols \
        --enable-protocol=file \
        --enable-protocol=rtp \
        --enable-protocol=tcp \
        --enable-protocol=udp \
        --enable-protocol=rtsp \
        --extra-cflags="-O3 -fPIC -std=c11 -fno-emulated-tls" \
        --extra-cxxflags="-std=c++11 -fno-emulated-tls" \
        --extra-ldflags="-lc -lm -ldl -llog -lz -Wl,--exclude-libs,ALL -Wl,--gc-sections" || exit 1

    echo "Starting make..."
    make clean
    make -j4
    make install

    # 创建合并的动态库 - 修改链接选项
    mkdir -p "$PREFIX/lib"

    # 检查静态库是否存在
    if [ ! -f "$PREFIX/lib/libavformat.a" ]; then
        echo "❌ Static libraries not found"
        return 1
    fi

    # 使用编译器创建动态库，添加TLS选项
    "$CC_CONFIGURE" -shared -fPIC \
        -fno-emulated-tls \
        -Wl,--no-undefined \
        -Wl,--export-dynamic \
        -Wl,--whole-archive \
        "$PREFIX/lib/libavformat.a" \
        "$PREFIX/lib/libavcodec.a" \
        "$PREFIX/lib/libavutil.a" \
        "$PREFIX/lib/libswresample.a" \
        "$PREFIX/lib/libswscale.a" \
        -Wl,--no-whole-archive \
        -o "$PREFIX/lib/libffmpeg.so" \
        -L"$ANDROID_NDK_ROOT_WIN/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/$HOST" \
        -L"$ANDROID_NDK_ROOT_WIN/platforms/android-$API/arch-$ARCH/usr/lib" \
        -landroid -lmediandk \
        -lc -lm -ldl -llog -lz

    # 移除所有GWP-ASan符号
    echo "Removing GWP-ASan symbols..."
    "$ANDROID_NDK_ROOT_WIN/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objcopy" \
        --wildcard --strip-symbol="*gwp_asan*" \
        "$PREFIX/lib/libffmpeg.so"

    # 验证GWP-ASan符号已被移除
    echo "Verifying GWP-ASan symbols removal..."
    GWP_COUNT=$("$ANDROID_NDK_ROOT_WIN/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-nm" "$PREFIX/lib/libffmpeg.so" 2>/dev/null | grep -c "gwp_asan" || echo "0")
    if [ "$GWP_COUNT" -eq 0 ]; then
        echo "✅ All GWP-ASan symbols removed successfully"
    else
        echo "⚠️  Warning: $GWP_COUNT GWP-ASan symbols still present"
        # 如果还有符号，尝试更彻底的移除
        echo "Attempting more thorough symbol removal..."
        "$ANDROID_NDK_ROOT_WIN/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip" \
            --strip-unneeded "$PREFIX/lib/libffmpeg.so"
    fi

    # 验证生成的库
    echo "Verifying generated library..."
    if [ -f "$PREFIX/lib/libffmpeg.so" ]; then
        # 检查库中的符号
        echo "Checking for FFmpeg symbols..."
        if "$ANDROID_NDK_ROOT_WIN/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-nm" -D "$PREFIX/lib/libffmpeg.so" 2>/dev/null | grep -E "av_|avformat_|avcodec_|avutil_" > /dev/null; then
            echo "✅ Library symbols verified successfully"
        else
            echo "❌ Library symbols verification failed"
            return 1
        fi
    else
        echo "❌ Library generation failed"
        return 1
    fi

    # 复制库文件到正确的位置
    DEST_DIR="$(pwd)/../app/libs/$ANDROID_ABI"
    mkdir -p "$DEST_DIR"
    cp "$PREFIX/lib/libffmpeg.so" "$DEST_DIR/"
    echo "✅ Copied library to $DEST_DIR"

    # 清理静态库
    rm -f "$PREFIX/lib/"*.a
    rm -rf "$PREFIX/bin" "$PREFIX/share"

    echo "编译完成 $CPU"
}

# arm64-v8a
ARCH=aarch64
CPU=armv8-a
HOST=aarch64-linux-android
ANDROID_ABI=arm64-v8a
CC="$TOOLCHAIN/bin/aarch64-linux-android$API-clang.cmd"
CXX="$TOOLCHAIN/bin/aarch64-linux-android$API-clang++.cmd"
CROSS_PREFIX="$TOOLCHAIN/bin/llvm-"
PREFIX="$(pwd)/app/src/main/cpp/ffmpeg/arm64-v8a"
build_android

# 注释掉其他架构的编译
: '
# armeabi-v7a
ARCH=arm
CPU=armv7-a
HOST=armv7a-linux-androideabi
CC="$TOOLCHAIN/bin/armv7a-linux-androideabi$API-clang.cmd"
CXX="$TOOLCHAIN/bin/armv7a-linux-androideabi$API-clang++.cmd"
CROSS_PREFIX="$TOOLCHAIN/bin/llvm-"
PREFIX="$(pwd)/app/src/main/cpp/ffmpeg/armeabi-v7a"
build_android

# x86
ARCH=x86
CPU=i686
HOST=i686-linux-android
CC="$TOOLCHAIN/bin/i686-linux-android$API-clang.cmd"
CXX="$TOOLCHAIN/bin/i686-linux-android$API-clang++.cmd"
CROSS_PREFIX="$TOOLCHAIN/bin/llvm-"
PREFIX="$(pwd)/app/src/main/cpp/ffmpeg/x86"
build_android

# x86_64
ARCH=x86_64
CPU=x86-64
HOST=x86_64-linux-android
CC="$TOOLCHAIN/bin/x86_64-linux-android$API-clang.cmd"
CXX="$TOOLCHAIN/bin/x86_64-linux-android$API-clang++.cmd"
CROSS_PREFIX="$TOOLCHAIN/bin/llvm-"
PREFIX="$(pwd)/app/src/main/cpp/ffmpeg/x86_64"
build_android
'

echo "编译完成！库文件位于: $(pwd)/app/src/main/cpp/ffmpeg"