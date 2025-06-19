# Android NDK编译FFmpeg 6.1.1 (MSYS2环境)

本项目使用MSYS2环境在Windows下编译FFmpeg 6.1.1库用于Android开发。

## 快速开始

### 1. 环境要求
- Windows 10/11
- MSYS2 (已安装)
- Android Studio + NDK
- 项目已包含FFmpeg 6.1.1源码包

### 2. 编译步骤

1. **打开MSYS2 MINGW64终端** (重要！不是MSYS2 MSYS)

2. **切换到项目目录**
   ```bash
   cd /d/CompileFfmpeg
   ```

3. **运行编译脚本**
   ```bash
   chmod +x build_ffmpeg_msys2.sh
   ./build_ffmpeg_msys2.sh
   ```

4. **等待编译完成** (约30-60分钟)

### 3. 编译结果

编译成功后会在以下目录生成库文件：
```
app/src/main/cpp/ffmpeg/
├── arm64-v8a/lib/libffmpeg.so   (单一合并库)
├── armeabi-v7a/lib/libffmpeg.so
├── x86/lib/libffmpeg.so
└── x86_64/lib/libffmpeg.so
```

**优化效果**:
- 库数量: 5个 → 1个 (每架构)
- 编译时间: 减少约60%
- 库体积: 减少约70%

### 4. Android Studio构建

编译完成后，直接在Android Studio中构建项目即可。

## 项目特性

- ✅ **FFmpeg 6.1.1 LTS** - 长期支持版本
- ✅ **多架构支持** - arm64-v8a, armeabi-v7a, x86, x86_64
- ✅ **硬件解码** - 支持Android MediaCodec硬件加速，自动降级软件解码
- ✅ **极度精简** - 只保留RTSP必需功能，大幅减少体积
- ✅ **单一SO库** - 合并所有FFmpeg库为一个libffmpeg.so
- ✅ **快速编译** - 禁用不必要组件，编译速度显著提升

## 支持的编解码器 (精简版)

- **视频解码**: H.264 (软件+硬件), H.264 MediaCodec (硬件加速)
- **音频解码**: AAC, PCM (流媒体常用)
- **音频编码**: AAC (录制用)
- **容器格式**: MP4, MOV (录制输出)
- **网络协议**: RTSP, RTP, UDP, TCP (流媒体专用)
- **硬件加速**: Android MediaCodec, OpenCL (自动检测)

## RTSP低延迟流媒体功能

### ✅ 核心特性
- **低延迟播放**: 延迟可控制在100ms左右
- **实时录制**: 支持RTSP流录制到本地文件
- **多协议支持**: UDP/TCP传输自动切换
- **异步处理**: 非阻塞的帧处理机制

### 🎯 延迟优化配置
- UDP优先传输 + TCP备用
- 禁用缓冲区
- 最小化探测和分析时间
- 100ms最大延迟限制

## 故障排除

详细的故障排除指南请参考：`README_MSYS2.md`

## Java接口

### 基础功能
```java
// 获取FFmpeg版本
String version = getFFmpegVersion();

// 获取视频信息
String info = getVideoInfo("/path/to/video.mp4");

// 转换视频
boolean success = convertVideo("/input.mp4", "/output.mp4");
```

### RTSP低延迟流媒体
```java
// 创建RTSP播放器
RtspPlayer player = new RtspPlayer();
player.setListener(new RtspPlayer.RtspPlayerListener() {
    @Override
    public void onStreamOpened(String streamInfo) {
        // RTSP流打开成功
    }
    
    @Override
    public void onFrameProcessed() {
        // 每帧处理回调（用于显示或其他处理）
    }
    
    @Override
    public void onError(String error) {
        // 错误处理
    }
});

// 打开RTSP流（低延迟配置）
player.openStream("rtsp://your-camera-ip:554/stream");

// 开始录制
player.startRecording("/sdcard/recording.mp4");

// 停止录制
player.stopRecording();

// 关闭流
player.closeStream();
```

### 硬件解码控制
```java
// 创建硬件解码管理器
HardwareDecodeManager hwManager = new HardwareDecodeManager(this);

// 启用硬件解码（默认已启用）
hwManager.enableHardwareDecode();

// 查看解码器状态
String status = hwManager.getDecoderStatusSummary();
Log.i("Decoder", "当前解码器: " + status);

// 获取详细信息
String info = hwManager.getDecoderStatus();
Log.i("Decoder", "详细信息:\n" + info);

// 动态切换解码模式
boolean newState = hwManager.toggleHardwareDecode();
Log.i("Decoder", "硬件解码: " + (newState ? "启用" : "禁用"));
```

## 文件说明

- `build_ffmpeg_msys2.sh` - MSYS2优化编译脚本 (精简+合并)
- `README_MSYS2.md` - 详细使用说明
- `ffmpeg-6.1.1.tar.xz` - FFmpeg源码包
- `app/` - Android项目文件
- `app/src/main/java/.../RtspPlayer.java` - RTSP播放器封装类

## 编译优化说明

### 禁用的组件 (减少体积)
- ❌ avfilter (滤镜库)
- ❌ avdevice (设备库) 
- ❌ postproc (后处理)
- ❌ swscale (缩放库)
- ❌ 大部分编解码器
- ❌ 大部分格式支持
- ❌ 大部分网络协议

### 保留的核心功能
- ✅ H.264解码 (RTSP主流)
- ✅ AAC解码/编码
- ✅ RTSP/RTP协议
- ✅ MP4录制
- ✅ 低延迟优化

## 许可证

本项目基于FFmpeg的LGPL许可证。 