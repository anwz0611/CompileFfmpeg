#!/bin/bash
# FFmpeg 动态库修复脚本 v2
# 专门解决符号冲突和静态库链接问题

set -e

echo "🔧 FFmpeg 动态库修复工具 v2"
echo "================================"

# ======================= 路径配置 =======================
NDK_25_PATH="C:/Users/pc/AppData/Local/Android/Sdk/ndk/25.2.9519653"
TOOLCHAIN_WIN="$NDK_25_PATH/toolchains/llvm/prebuilt/windows-x86_64"
SYSROOT_WIN="$TOOLCHAIN_WIN/sysroot"
API=24
ANDROID_ABI="arm64-v8a"
HOST="aarch64-linux-android"

# 查找FFmpeg构建目录
FFMPEG_BUILD_DIR=""
if [ -d "ffmpeg-6.1.1/build/$ANDROID_ABI" ]; then
    FFMPEG_BUILD_DIR="ffmpeg-6.1.1/build/$ANDROID_ABI"
elif [ -d "build/$ANDROID_ABI" ]; then
    FFMPEG_BUILD_DIR="build/$ANDROID_ABI"
else
    echo "❌ 找不到FFmpeg构建目录"
    exit 1
fi

PREFIX="$(pwd)/$FFMPEG_BUILD_DIR"
echo "📁 使用构建目录: $PREFIX"

# 编译器配置
CC="$TOOLCHAIN_WIN/bin/${HOST}${API}-clang"
AR="$TOOLCHAIN_WIN/bin/llvm-ar"
NM="$TOOLCHAIN_WIN/bin/llvm-nm"

# ======================= 检查静态库 =======================
echo "📊 检查静态库状态..."
STATIC_LIBS=(
    "libavformat.a"
    "libavcodec.a"
    "libavutil.a"
    "libswresample.a"
    "libswscale.a"
)

for lib in "${STATIC_LIBS[@]}"; do
    LIB_PATH="$PREFIX/lib/$lib"
    if [ -f "$LIB_PATH" ]; then
        LIB_SIZE=$(stat -c%s "$LIB_PATH" 2>/dev/null || echo "0")
        echo "  ✅ $lib: $(($LIB_SIZE / 1024))KB"
    else
        echo "  ❌ $lib: 缺失"
        exit 1
    fi
done

# ======================= 方法1: 允许重复符号 =======================
echo ""
echo "🔗 尝试方法1: 允许重复符号定义..."
COMBINED_SO="$PREFIX/lib/libffmpeg.so"

# 备份原有动态库
if [ -f "$COMBINED_SO" ]; then
    cp "$COMBINED_SO" "$COMBINED_SO.backup"
fi

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
    -landroid -lmediandk -llog -lz -lm -lc++ \
    -Wl,--no-undefined \
    -o "$COMBINED_SO" 2>&1 | tee link_log_v2.txt

# 检查方法1结果
if [ -f "$COMBINED_SO" ]; then
    SO_SIZE=$(stat -c%s "$COMBINED_SO" 2>/dev/null || echo "0")
    echo "📊 方法1动态库大小: $SO_SIZE bytes ($(($SO_SIZE / 1024 / 1024))MB)"

    if [ "$SO_SIZE" -gt 5000000 ]; then  # 至少5MB
        echo "✅ 方法1成功: 动态库大小正常"
        METHOD1_SUCCESS=true
    else
        echo "⚠️  方法1生成的库太小，尝试其他方法"
        METHOD1_SUCCESS=false
    fi
else
    echo "❌ 方法1失败"
    METHOD1_SUCCESS=false
fi

# ======================= 方法2: 去重合并 =======================
if [ "$METHOD1_SUCCESS" != "true" ]; then
    echo ""
    echo "🔗 尝试方法2: 符号去重合并..."

    # 创建临时目录
    TEMP_DIR="$PREFIX/temp_merge"
    rm -rf "$TEMP_DIR" && mkdir -p "$TEMP_DIR"

    # 解压所有静态库到临时目录
    echo "📦 解压静态库..."
    for lib in "${STATIC_LIBS[@]}"; do
        LIB_DIR="$TEMP_DIR/$(basename $lib .a)"
        mkdir -p "$LIB_DIR"
        cd "$LIB_DIR"
        "$AR" x "$PREFIX/lib/$lib"
        echo "  ✅ 解压 $lib"
        cd - > /dev/null
    done

    # 收集所有对象文件，处理重复
    echo "🔄 处理重复符号..."
    OBJECT_FILES=()
    PROCESSED_FILES=()

    for lib_dir in "$TEMP_DIR"/*; do
        if [ -d "$lib_dir" ]; then
            for obj_file in "$lib_dir"/*.o; do
                if [ -f "$obj_file" ]; then
                    obj_name=$(basename "$obj_file")

                    # 检查是否已经处理过同名文件
                    found=false
                    for processed in "${PROCESSED_FILES[@]}"; do
                        if [ "$processed" = "$obj_name" ]; then
                            echo "  ⚠️  跳过重复文件: $obj_name"
                            found=true
                            break
                        fi
                    done

                    if [ "$found" = false ]; then
                        OBJECT_FILES+=("$obj_file")
                        PROCESSED_FILES+=("$obj_name")
                    fi
                fi
            done
        fi
    done

    echo "📊 收集到 ${#OBJECT_FILES[@]} 个唯一对象文件"

    # 创建合并的动态库
    echo "🔗 链接对象文件..."
    "$CC" -shared -fPIC \
        "${OBJECT_FILES[@]}" \
        -L"$SYSROOT_WIN/usr/lib/aarch64-linux-android/$API" \
        -L"$TOOLCHAIN_WIN/sysroot/usr/lib/aarch64-linux-android/$API" \
        -landroid -lmediandk -llog -lz -lm -lc++ \
        -o "$COMBINED_SO" 2>&1 | tee -a link_log_v2.txt

    # 清理临时目录
    rm -rf "$TEMP_DIR"

    # 检查方法2结果
    if [ -f "$COMBINED_SO" ]; then
        SO_SIZE=$(stat -c%s "$COMBINED_SO" 2>/dev/null || echo "0")
        echo "📊 方法2动态库大小: $SO_SIZE bytes ($(($SO_SIZE / 1024 / 1024))MB)"

        if [ "$SO_SIZE" -gt 5000000 ]; then
            echo "✅ 方法2成功"
            METHOD2_SUCCESS=true
        else
            echo "⚠️  方法2库仍然较小"
            METHOD2_SUCCESS=false
        fi
    else
        echo "❌ 方法2失败"
        METHOD2_SUCCESS=false
    fi
fi

# ======================= 方法3: 链接顺序优化 =======================
if [ "$METHOD1_SUCCESS" != "true" ] && [ "$METHOD2_SUCCESS" != "true" ]; then
    echo ""
    echo "🔗 尝试方法3: 优化链接顺序..."

    # 按依赖关系重新排序库
    "$CC" -shared -fPIC \
        -Wl,--allow-multiple-definition \
        -Wl,--start-group \
        "$PREFIX/lib/libavformat.a" \
        "$PREFIX/lib/libavcodec.a" \
        "$PREFIX/lib/libavutil.a" \
        "$PREFIX/lib/libswresample.a" \
        "$PREFIX/lib/libswscale.a" \
        -Wl,--end-group \
        -L"$SYSROOT_WIN/usr/lib/aarch64-linux-android/$API" \
        -L"$TOOLCHAIN_WIN/sysroot/usr/lib/aarch64-linux-android/$API" \
        -landroid -lmediandk -llog -lz -lm -lc++ \
        -Wl,--no-undefined \
        -o "$COMBINED_SO" 2>&1 | tee -a link_log_v2.txt

    if [ -f "$COMBINED_SO" ]; then
        SO_SIZE=$(stat -c%s "$COMBINED_SO" 2>/dev/null || echo "0")
        echo "📊 方法3动态库大小: $SO_SIZE bytes ($(($SO_SIZE / 1024 / 1024))MB)"

        if [ "$SO_SIZE" -gt 5000000 ]; then
            echo "✅ 方法3成功"
            METHOD3_SUCCESS=true
        else
            echo "⚠️  方法3库仍然较小"
            METHOD3_SUCCESS=false
        fi
    else
        echo "❌ 方法3失败"
        METHOD3_SUCCESS=false
    fi
fi

# ======================= 验证最终结果 =======================
if [ -f "$COMBINED_SO" ]; then
    echo ""
    echo "🔍 验证生成的动态库..."

    SO_SIZE=$(stat -c%s "$COMBINED_SO" 2>/dev/null || echo "0")
    echo "📊 最终大小: $SO_SIZE bytes ($(($SO_SIZE / 1024 / 1024))MB)"

    # 检查符号
    SYMBOL_COUNT=$("$NM" -D "$COMBINED_SO" 2>/dev/null | wc -l || echo "0")
    echo "📊 导出符号数量: $SYMBOL_COUNT"

    # 检查关键符号
    KEY_SYMBOLS=(
        "avformat_open_input"
        "avcodec_find_decoder"
        "av_read_frame"
        "avformat_network_init"
        "avcodec_alloc_context3"
    )

    FOUND_SYMBOLS=0
    for symbol in "${KEY_SYMBOLS[@]}"; do
        if "$NM" -D "$COMBINED_SO" 2>/dev/null | grep -q "$symbol"; then
            echo "  ✅ $symbol"
            FOUND_SYMBOLS=$((FOUND_SYMBOLS + 1))
        else
            echo "  ❌ $symbol"
        fi
    done

    # 复制到项目目录
    echo ""
    echo "📦 复制到项目目录..."
    TARGET_DIR="app/src/main/cpp/ffmpeg/$ANDROID_ABI"
    mkdir -p "$TARGET_DIR/lib"
    cp "$COMBINED_SO" "$TARGET_DIR/lib/"

    # 最终评估
    echo ""
    echo "================================"
    if [ "$SO_SIZE" -gt 5000000 ] && [ "$FOUND_SYMBOLS" -ge 3 ]; then
        echo "🎉 动态库修复成功!"
        echo "📊 大小: $(($SO_SIZE / 1024 / 1024))MB"
        echo "📊 符号: $FOUND_SYMBOLS/${#KEY_SYMBOLS[@]} 个关键符号"
        echo "📁 位置: $TARGET_DIR/lib/libffmpeg.so"
    else
        echo "⚠️  动态库可能存在问题"
        echo "📊 大小: $(($SO_SIZE / 1024 / 1024))MB (期望>5MB)"
        echo "📊 符号: $FOUND_SYMBOLS/${#KEY_SYMBOLS[@]} 个关键符号"
        echo "💡 请检查链接日志: link_log_v2.txt"
    fi
else
    echo "❌ 所有方法都失败了"
    echo "💡 请检查链接日志: link_log_v2.txt"
    exit 1
fi