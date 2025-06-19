# MSYS2环境下编译FFmpeg for Android

本指南专门针对Windows系统下使用MSYS2环境编译FFmpeg的用户。

## 为什么选择FFmpeg 6.1.1？

- ✅ **LTS版本**: 长期支持，稳定可靠
- ✅ **性能优化**: 相比5.1版本有显著性能提升
- ✅ **Bug修复**: 修复了大量已知问题
- ✅ **新特性**: 支持更多编解码器和格式
- ✅ **安全性**: 修复了多个安全漏洞

如果你坚持使用5.1版本，可以修改脚本中的`FFMPEG_VERSION="6.1.1"`为`FFMPEG_VERSION="5.1.4"`。

## 环境准备

### 1. 安装MSYS2

1. 从 [MSYS2官网](https://www.msys2.org/) 下载并安装
2. 安装完成后，打开 "MSYS2 MINGW64" 终端（重要：不是MSYS2 MSYS终端）
3. 更新包管理器：
   ```bash
   pacman -Syu
   ```

### 2. 安装Android SDK和NDK

确保你已经安装了：
- Android Studio
- Android SDK
- Android NDK (推荐26.1.10909125或更新版本)

### 3. 设置环境变量（可选）

在 `~/.bashrc` 文件中添加（或在运行时手动输入）：
```bash
export ANDROID_HOME="/c/Users/YourUsername/AppData/Local/Android/Sdk"
export ANDROID_NDK_HOME="$ANDROID_HOME/ndk/26.1.10909125"
```

**注意**: 将路径替换为你实际的Android SDK路径。

## 编译步骤

### 1. 准备项目

```bash
# 切换到项目目录
cd /d/CompileFfmpeg

# 给脚本执行权限
chmod +x build_ffmpeg_msys2.sh
```

### 2. 运行编译脚本

```bash
./build_ffmpeg_msys2.sh
```

脚本会自动：
- 检查MSYS2环境
- 安装必要的编译工具
- 自动检测Android SDK和NDK路径
- 下载FFmpeg 6.1.1源码
- 编译4个架构版本（arm64-v8a, armeabi-v7a, x86, x86_64）

### 3. 编译过程

编译过程大约需要30-60分钟，取决于你的电脑性能。你会看到类似的输出：

```
=== FFmpeg Android NDK 编译脚本 (MSYS2版本) ===
推荐的FFmpeg版本: 6.1.1 (LTS)

检测到MSYS2环境: MINGW64
安装必要的编译工具...
环境配置:
  ANDROID_HOME: /c/Users/YourName/AppData/Local/Android/Sdk
  ANDROID_NDK_HOME: /c/Users/YourName/AppData/Local/Android/Sdk/ndk/26.1.10909125
  NDK版本: 26.1.10909125
  工具链: /c/Users/YourName/AppData/Local/Android/Sdk/ndk/26.1.10909125/toolchains/llvm/prebuilt/windows-x86_64

FFmpeg版本: 6.1.1
输出目录: /d/CompileFfmpeg/app/src/main/cpp/ffmpeg

===== 开始编译 arm64-v8a 架构 =====
...
```

## 常见问题及解决方案

### 1. "错误: 请在MSYS2环境中运行此脚本"

**原因**: 在错误的终端中运行脚本  
**解决**: 打开 "MSYS2 MINGW64" 终端，不是普通的Windows CMD或PowerShell

### 2. "错误: 未找到Android NDK"

**原因**: Android SDK/NDK路径不正确  
**解决**: 
- 检查Android Studio是否正确安装NDK
- 手动设置环境变量或在脚本提示时输入正确路径

### 3. "编译器不存在"

**原因**: NDK工具链路径问题  
**解决**: 
- 确保使用的是Windows版本的NDK
- 检查NDK版本是否兼容

### 4. 下载失败

**原因**: 网络连接问题  
**解决**: 
- 使用VPN或科学上网工具
- 手动下载FFmpeg源码并解压到项目目录

### 5. 编译速度慢

**优化建议**:
- 关闭不必要的程序释放CPU和内存
- 使用SSD硬盘
- 增加虚拟内存

## 编译选项自定义

如果你需要自定义编译选项，可以修改 `build_ffmpeg_msys2.sh` 中的configure参数：

```bash
# 示例：启用更多编解码器
./configure \
    --enable-decoder=vp8 \
    --enable-decoder=vp9 \
    --enable-encoder=libvpx \
    # ... 其他选项
```

## 验证编译结果

编译成功后，检查以下目录：
```
app/src/main/cpp/ffmpeg/
├── arm64-v8a/
│   ├── include/
│   └── lib/
│       ├── libavcodec.so
│       ├── libavformat.so
│       ├── libavutil.so
│       ├── libswscale.so
│       └── libswresample.so
├── armeabi-v7a/
├── x86/
└── x86_64/
```

## 下一步

1. 在Android Studio中构建项目
2. 运行应用测试FFmpeg功能
3. 根据需要调整编译配置

## 性能对比

| 版本 | 编解码性能 | 库大小 | 兼容性 | 安全性 |
|------|------------|--------|--------|--------|
| 5.1.4 | 基准 | 较大 | 较好 | 一般 |
| 6.1.1 | +15-20% | 优化 | 更好 | 更安全 |

## 故障排除

如果遇到问题，请提供以下信息：
1. MSYS2版本: `pacman -Q msys2-runtime`
2. NDK版本: `ls $ANDROID_NDK_HOME`
3. 错误日志: 完整的终端输出
4. 系统信息: Windows版本

## 技术支持

- 检查项目的README_FFmpeg.md了解更多详情
- 查看官方FFmpeg文档
- 搜索相关的Android NDK问题 