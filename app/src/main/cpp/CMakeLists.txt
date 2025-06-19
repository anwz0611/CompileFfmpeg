# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html.
# For more examples on how to use CMake, see https://github.com/android/ndk-samples.

# Sets the minimum CMake version required for this project.
cmake_minimum_required(VERSION 3.22.1)

# Declares the project name. The project name can be accessed via ${ PROJECT_NAME},
# Since this is the top level CMakeLists.txt, the project name is also accessible
# with ${CMAKE_PROJECT_NAME} (both CMake variables are in-sync within the top level
# build script scope).
project("CompileFfmpeg")

# 设置FFmpeg路径（支持多种方式） 
# 方式1：项目根目录下的ffmpeg-6.1.1编译结果
set(FFMPEG_ROOT_DIR ${CMAKE_SOURCE_DIR}/../../../../ffmpeg-6.1.1)
set(FFMPEG_INCLUDE_DIR ${FFMPEG_ROOT_DIR}/app/src/main/cpp/ffmpeg/${ANDROID_ABI}/include)
set(FFMPEG_LIBS_DIR ${FFMPEG_ROOT_DIR}/app/src/main/cpp/ffmpeg/${ANDROID_ABI}/lib)

# 方式2：cpp目录下的ffmpeg目录
set(LOCAL_FFMPEG_DIR ${CMAKE_SOURCE_DIR}/ffmpeg)
set(LOCAL_FFMPEG_INCLUDE_DIR ${LOCAL_FFMPEG_DIR}/${ANDROID_ABI}/include)
set(LOCAL_FFMPEG_LIBS_DIR ${LOCAL_FFMPEG_DIR}/${ANDROID_ABI}/lib)

# 方式3：jniLibs目录（预编译库）
set(JNI_LIBS_DIR ${CMAKE_SOURCE_DIR}/../../../libs/${ANDROID_ABI})

message(STATUS "Looking for FFmpeg libraries for ${ANDROID_ABI}")
message(STATUS "JNI_LIBS_DIR: ${JNI_LIBS_DIR}")
message(STATUS "FFMPEG_INCLUDE_DIR: ${FFMPEG_INCLUDE_DIR}")
message(STATUS "LOCAL_FFMPEG_INCLUDE_DIR: ${LOCAL_FFMPEG_INCLUDE_DIR}")

# 检查预编译库 + 根目录头文件的组合（推荐方式）
if(EXISTS ${JNI_LIBS_DIR}/libffmpeg.so AND EXISTS ${FFMPEG_INCLUDE_DIR})
    # 使用jniLibs中的预编译FFmpeg库 + 根目录的头文件
    add_library(ffmpeg SHARED IMPORTED)
    set_target_properties(ffmpeg PROPERTIES IMPORTED_LOCATION ${JNI_LIBS_DIR}/libffmpeg.so)
    
    # 添加头文件目录
    include_directories(${FFMPEG_INCLUDE_DIR})
    
    message(STATUS "✅ Found precompiled FFmpeg library in jniLibs + headers in ffmpeg-6.1.1")
    set(FFMPEG_LIBRARIES ffmpeg)
    add_definitions(-DFFMPEG_FOUND=1)
    
    # 确保FFmpeg库被安装到APK中
    set_target_properties(ffmpeg PROPERTIES
        IMPORTED_LOCATION ${JNI_LIBS_DIR}/libffmpeg.so
        IMPORTED_NO_SONAME ON)

# 检查本地cpp目录的完整FFmpeg
elseif(EXISTS ${LOCAL_FFMPEG_INCLUDE_DIR})
    include_directories(${LOCAL_FFMPEG_INCLUDE_DIR})
    
    # 检查是否有合并的FFmpeg库
    if(EXISTS ${LOCAL_FFMPEG_LIBS_DIR}/libffmpeg.so)
        add_library(ffmpeg SHARED IMPORTED)
        set_target_properties(ffmpeg PROPERTIES IMPORTED_LOCATION ${LOCAL_FFMPEG_LIBS_DIR}/libffmpeg.so)
        set(FFMPEG_LIBRARIES ffmpeg)
        message(STATUS "✅ Found merged FFmpeg library in cpp/ffmpeg")
    else()
        # 使用分离的FFmpeg库
        if(EXISTS ${LOCAL_FFMPEG_LIBS_DIR}/libavcodec.so)
            add_library(avcodec SHARED IMPORTED)
            set_target_properties(avcodec PROPERTIES IMPORTED_LOCATION ${LOCAL_FFMPEG_LIBS_DIR}/libavcodec.so)
            
            add_library(avformat SHARED IMPORTED)
            set_target_properties(avformat PROPERTIES IMPORTED_LOCATION ${LOCAL_FFMPEG_LIBS_DIR}/libavformat.so)
            
            add_library(avutil SHARED IMPORTED)
            set_target_properties(avutil PROPERTIES IMPORTED_LOCATION ${LOCAL_FFMPEG_LIBS_DIR}/libavutil.so)
            
            add_library(swresample SHARED IMPORTED)
            set_target_properties(swresample PROPERTIES IMPORTED_LOCATION ${LOCAL_FFMPEG_LIBS_DIR}/libswresample.so)
            
            set(FFMPEG_LIBRARIES avcodec avformat avutil swresample)
            message(STATUS "✅ Found separate FFmpeg libraries in cpp/ffmpeg")
        endif()
    endif()
    
    add_definitions(-DFFMPEG_FOUND=1)

# 最后尝试仅使用预编译库（无头文件）
elseif(EXISTS ${JNI_LIBS_DIR}/libffmpeg.so)
    add_library(ffmpeg SHARED IMPORTED)
    set_target_properties(ffmpeg PROPERTIES IMPORTED_LOCATION ${JNI_LIBS_DIR}/libffmpeg.so)
    set(FFMPEG_LIBRARIES ffmpeg)
    add_definitions(-DFFMPEG_FOUND=1)
    message(STATUS "⚠️  Found FFmpeg library but no headers - some features may not work")

else()
    message(WARNING "❌ FFmpeg not found for ${ANDROID_ABI}")
    message(WARNING "Please ensure either:")
    message(WARNING "1. Compiled libraries in cpp/ffmpeg/${ANDROID_ABI}/")
    message(WARNING "2. Precompiled .so files in libs/${ANDROID_ABI}/")
    message(WARNING "3. Header files in appropriate include directories")
    add_definitions(-DFFMPEG_FOUND=0)
endif()

# Creates and names a library, sets it as either STATIC
# or SHARED, and provides the relative paths to its source code.
# You can define multiple libraries, and CMake builds them for you.
# Gradle automatically packages shared libraries with your APK.
#
# In this top level CMakeLists.txt, ${CMAKE_PROJECT_NAME} is used to define
# the target library name; in the sub-module's CMakeLists.txt, ${PROJECT_NAME}
# is preferred for the same purpose.
#
# In order to load a library into your app from Java/Kotlin, you must call
# System.loadLibrary() and pass the name of the library defined here;
# for GameActivity/NativeActivity derived applications, the same library name must be
# used in the AndroidManifest.xml file.
add_library(${CMAKE_PROJECT_NAME} SHARED
        # List C/C++ source files with relative paths to this CMakeLists.txt.
        ffmpeg_wrapper.cpp)

# Specifies libraries CMake should link to your target library.
if(DEFINED FFMPEG_LIBRARIES)
    target_link_libraries(${CMAKE_PROJECT_NAME}
            # Android system libraries first
            android
            log
            z
            m
            dl
            c++_shared
            # FFmpeg libraries after system libraries
            ${FFMPEG_LIBRARIES})
    message(STATUS "✅ Linking with FFmpeg libraries: ${FFMPEG_LIBRARIES}")
else()
    target_link_libraries(${CMAKE_PROJECT_NAME}
            # Basic Android libraries only
            android
            log
            c++_shared)
    message(STATUS "⚠️  Linking without FFmpeg libraries")
endif()