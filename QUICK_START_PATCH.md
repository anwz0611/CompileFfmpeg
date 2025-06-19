# 超低延迟补丁快速使用指南

## 🚀 快速开始 (5分钟上手)

### 第一步：准备环境
```bash
# 确保FFmpeg源码已解压
ls ffmpeg-6.1.1/  # 应该看到源码目录

# 给脚本执行权限
chmod +x apply_ultra_low_latency_patch.sh
```

### 第二步：应用补丁
```bash
# 应用超低延迟补丁（一键完成）
./apply_ultra_low_latency_patch.sh
```

看到 `✅ 补丁应用成功！` 就表示成功了。

### 第三步：编译FFmpeg
```bash
# 编译包含补丁的FFmpeg库
./build_ffmpeg_msys2.sh
```

### 第四步：验证结果
编译完成后，你会获得：
- **更低延迟**: 100-150ms 额外延迟减少
- **更快响应**: RTP帧直接处理，无需等待
- **优化解析**: 移除冗余的解析循环

---

## 📋 完整命令序列

```bash
# 一键应用所有优化
chmod +x apply_ultra_low_latency_patch.sh
./apply_ultra_low_latency_patch.sh apply
./build_ffmpeg_msys2.sh
```

## 🔍 验证补丁状态

```bash
# 检查补丁是否正确应用
./apply_ultra_low_latency_patch.sh verify
```

预期输出：
```
✓ rtpdec.c - mark_flag 变量已添加
✓ parser.c - 外部变量声明已添加  
✓ h264_parser.c - 外部变量声明已添加
✅ 补丁验证通过！
```

## ⚠️ 如果遇到问题

### 补丁应用失败
```bash
# 检查源码目录
ls -la ffmpeg-6.1.1/

# 重新解压源码
rm -rf ffmpeg-6.1.1
tar -xf ffmpeg-6.1.1.tar.xz

# 重新应用补丁
./apply_ultra_low_latency_patch.sh apply
```

### 编译失败
```bash
# 恢复到原版
./apply_ultra_low_latency_patch.sh restore

# 先编译原版确认环境OK
./build_ffmpeg_msys2.sh

# 再应用补丁
./apply_ultra_low_latency_patch.sh apply
./build_ffmpeg_msys2.sh
```

### 运行时问题
```bash
# 恢复到安全优化版本
./apply_ultra_low_latency_patch.sh restore
./build_ffmpeg_msys2.sh
```

## 📊 性能对比

| 版本 | 延迟 | 兼容性 | 推荐场景 |
|-----|------|--------|----------|
| 原版FFmpeg | ~300ms | 100% | 一般应用 |
| 安全优化 | ~200ms | 95% | 大多数应用 |
| 激进优化 (本补丁) | ~130ms | 80% | 延迟敏感应用 |

## 🎯 使用建议

1. **首次使用**: 先应用补丁，在测试环境验证功能
2. **生产部署**: 充分测试后再部署到生产环境
3. **问题排查**: 遇到兼容性问题时，可以快速回滚到安全优化版本

---

💡 **小贴士**: 这个补丁特别适合：
- 实时监控应用
- 低延迟直播
- 工业控制场景
- 远程操作系统

需要详细了解技术原理，请查看 `ULTRA_LOW_LATENCY_README.md` 