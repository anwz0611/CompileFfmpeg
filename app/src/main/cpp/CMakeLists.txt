# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html.
# For more examples on how to use CMake, see https://github.com/android/ndk-samples.

# Sets the minimum CMake version required for this project.
cmake_minimum_required(VERSION 3.22.1)

# 设置编译选项 - 针对NDK 21优化
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC -O2")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC -std=c++11 -O2")

# Android API 级别检查 - 支持API 21+
if("${ANDROID_PLATFORM}" STREQUAL "")
    set(ANDROID_PLATFORM "android-21")
endif()
string(REPLACE "android-" "" ANDROID_PLATFORM_LEVEL ${ANDROID_PLATFORM})
if(ANDROID_PLATFORM_LEVEL LESS 21)
    message(FATAL_ERROR "Minimum Android API level required is 21")
endif()
message(STATUS "Using Android API level: ${ANDROID_PLATFORM_LEVEL}")

# Declares the project name
project("CompileFfmpeg")

# NDK版本检查和优化
message(STATUS "NDK Version: ${ANDROID_NDK_REVISION}")
message(STATUS "Android ABI: ${ANDROID_ABI}")
message(STATUS "Android Platform: ${ANDROID_PLATFORM}")

# 设置FFmpeg路径
set(FFMPEG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg/${ANDROID_ABI})
set(FFMPEG_INCLUDE_DIR ${FFMPEG_DIR}/include)
set(FFMPEG_LIB_DIR ${FFMPEG_DIR}/lib)
set(FFMPEG_SO_PATH ${FFMPEG_LIB_DIR}/libffmpeg.so)

# 检查FFmpeg库和头文件
message(STATUS "Looking for FFmpeg in: ${FFMPEG_DIR}")
message(STATUS "Include dir: ${FFMPEG_INCLUDE_DIR}")
message(STATUS "Lib dir: ${FFMPEG_LIB_DIR}")
message(STATUS "SO path: ${FFMPEG_SO_PATH}")

if(EXISTS ${FFMPEG_INCLUDE_DIR} AND EXISTS ${FFMPEG_SO_PATH})
    message(STATUS "✅ Found FFmpeg in ${FFMPEG_DIR}")
    
    # 添加头文件路径
    include_directories(${FFMPEG_INCLUDE_DIR})
    
    # 导入FFmpeg库（作为PRIVATE依赖）
    add_library(ffmpeg SHARED IMPORTED)
    set_target_properties(ffmpeg PROPERTIES
        IMPORTED_LOCATION ${FFMPEG_SO_PATH}
        IMPORTED_SONAME "libffmpeg.so"
        INTERFACE_INCLUDE_DIRECTORIES ${FFMPEG_INCLUDE_DIR})
    
    # 确保FFmpeg库被包含在APK中
    set_target_properties(ffmpeg PROPERTIES
        IMPORTED_NO_SONAME TRUE)
    
    # 验证库文件大小（基本健康检查）
    file(SIZE ${FFMPEG_SO_PATH} FFMPEG_LIB_SIZE)
    if(FFMPEG_LIB_SIZE GREATER 500000)  # 至少500KB
        message(STATUS "✅ FFmpeg library size: ${FFMPEG_LIB_SIZE} bytes")
    set(FFMPEG_FOUND TRUE)
    else()
        message(WARNING "❌ FFmpeg library seems too small: ${FFMPEG_LIB_SIZE} bytes")
        set(FFMPEG_FOUND FALSE)
    endif()
else()
    message(WARNING "❌ FFmpeg not found in ${FFMPEG_DIR}")
    message(STATUS "Include dir exists: ${EXISTS ${FFMPEG_INCLUDE_DIR}}")
    message(STATUS "SO file exists: ${EXISTS ${FFMPEG_SO_PATH}}")
    set(FFMPEG_FOUND FALSE)
endif()

# 创建主库 CompileFfmpeg.so
add_library(${CMAKE_PROJECT_NAME} SHARED
    ffmpeg_wrapper.cpp)

# 设置编译定义
if(FFMPEG_FOUND)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE FFMPEG_FOUND=1)
else()
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE FFMPEG_FOUND=0)
endif()

# 设置目标属性
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON)

# 链接库 - 新的架构设计
if(FFMPEG_FOUND)
    message(STATUS "✅ Linking with FFmpeg and system libraries")
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
        # FFmpeg库（作为PRIVATE依赖）
        ffmpeg
        # Android系统库（处理ANativeWindow等系统功能）
        android
        mediandk
        log
        z
        m
        dl
        # 其他系统库
        OpenSLES
        c++_shared  # 修改为 c++_shared 以匹配 FFmpeg 的依赖
        atomic)
else()
    message(STATUS "⚠️  Building without FFmpeg support")
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
        android
        log
        c++_shared)  # 修改为 c++_shared
endif()

# 构建后验证和库复制
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "✅ Build completed successfully"
    COMMAND ${CMAKE_COMMAND} -E echo "📁 Output: $<TARGET_FILE:${CMAKE_PROJECT_NAME}>"
    COMMENT "Build verification"
)

# 如果找到FFmpeg，确保库文件被复制到输出目录
if(FFMPEG_FOUND)
    add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${FFMPEG_SO_PATH}
        $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/libffmpeg.so
        COMMENT "Copying FFmpeg library to output directory"
    )
endif()