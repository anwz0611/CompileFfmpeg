@echo off
echo 正在修复FFmpeg库加载问题...

REM 检查是否有Java 11
echo 检查Java版本...
java -version

REM 设置JAVA_HOME到Java 11 (如果你有的话)
REM set JAVA_HOME=C:\Program Files\Java\jdk-11.0.XX
REM set PATH=%JAVA_HOME%\bin;%PATH%

echo.
echo 如果你没有Java 11，请下载并安装：
echo https://adoptium.net/temurin/releases/?version=11
echo.

echo 清理并重新构建项目...
call gradlew clean assembleDebug

echo.
echo 构建完成！如果成功，请重新安装APK并测试。
pause 