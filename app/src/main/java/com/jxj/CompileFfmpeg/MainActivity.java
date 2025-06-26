package com.jxj.CompileFfmpeg;

import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;

import android.Manifest;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.text.format.DateFormat;
import android.util.Log;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;
import android.view.SurfaceView;
import android.view.Surface;

import com.jxj.CompileFfmpeg.databinding.ActivityMainBinding;

import java.io.File;
import java.util.Date;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class MainActivity extends AppCompatActivity {

    private static final String TAG = "MainActivity";
    
        // Used to load the 'CompileFfmpeg' library on application startup.
    static {
        try {
//            System.loadLibrary("ffmpeg");
            System.loadLibrary("CompileFfmpeg");
            Log.d(TAG, "✅ 成功加载CompileFfmpeg库");
            
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "❌ 无法加载native库: " + e.getMessage());
            Log.e(TAG, "❌ 详细错误信息: " + e.toString());
            // 打印更多调试信息
            try {
                String libraryPath = System.getProperty("java.library.path");
                Log.d(TAG, "Library path: " + libraryPath);
                
                String dataDir = System.getProperty("android.app.lib.0");
                Log.d(TAG, "App lib directory: " + dataDir);
            } catch (Exception ex) {
                Log.e(TAG, "无法获取library path: " + ex.getMessage());
            }
            
            throw e;
        }
    }

    private ActivityMainBinding binding;
    private static final int PERMISSION_REQUEST_CODE = 1;
    
    // UI 组件
    private EditText etRtspUrl;
    private Button btnConnect, btnStartTest, btnRecord, btnDisconnect, btnClearLog;
    private TextView tvDecodeLatency, tvFps, tvNetworkLatency, tvTotalLatency;
    private TextView tvDecoderInfo, tvLog;
    private Switch swHardwareDecode;
    private ScrollView logScrollView;
    private SurfaceView surfaceView;
    private TextView tvNoVideo;
    
    // RTSP 相关
    private RtspPlayer rtspPlayer;
    private boolean isConnected = false;
    private boolean isTesting = false;
    private boolean isRecording = false;
    
    // 性能监控
    private Handler mainHandler;
    private ExecutorService executorService;
    private PerformanceMonitor performanceMonitor;
    
    // 延迟测试相关
    private long testStartTime;
    private int frameCount = 0;
    private long lastFpsTime = 0;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        binding = ActivityMainBinding.inflate(getLayoutInflater());
        setContentView(binding.getRoot());

        // 初始化组件
        initializeComponents();
        
        // 检查权限
        checkPermissions();

        // 设置UI
        setupUI();
        
        // 初始化RTSP播放器
        initializeRtspPlayer();
    }

    private void checkPermissions() {
        // 检查必要权限
        String[] permissions = {
            Manifest.permission.READ_EXTERNAL_STORAGE,
            Manifest.permission.WRITE_EXTERNAL_STORAGE,
            Manifest.permission.INTERNET,
            Manifest.permission.ACCESS_NETWORK_STATE,
            Manifest.permission.ACCESS_WIFI_STATE
        };
        
        boolean needRequest = false;
        for (String permission : permissions) {
            if (ContextCompat.checkSelfPermission(this, permission) != PackageManager.PERMISSION_GRANTED) {
                needRequest = true;
                break;
            }
        }
        
        if (needRequest) {
            ActivityCompat.requestPermissions(this, permissions, PERMISSION_REQUEST_CODE);
        }
    }

    private void initializeComponents() {
        // 初始化Handler和线程池
        mainHandler = new Handler(Looper.getMainLooper());
        executorService = Executors.newCachedThreadPool();
        
        // 绑定UI组件
        etRtspUrl = findViewById(R.id.et_rtsp_url);
        btnConnect = findViewById(R.id.btn_connect);
        btnStartTest = findViewById(R.id.btn_start_test);
        btnRecord = findViewById(R.id.btn_record);
        btnDisconnect = findViewById(R.id.btn_disconnect);
        btnClearLog = findViewById(R.id.btn_clear_log);
        
        tvDecodeLatency = findViewById(R.id.tv_decode_latency);
        tvFps = findViewById(R.id.tv_fps);
        tvNetworkLatency = findViewById(R.id.tv_network_latency);
        tvTotalLatency = findViewById(R.id.tv_total_latency);
        tvDecoderInfo = findViewById(R.id.tv_decoder_info);
        tvLog = findViewById(R.id.tv_log);
        
        swHardwareDecode = findViewById(R.id.sw_hardware_decode);
        logScrollView = findViewById(R.id.tv_log).getParent() instanceof ScrollView ? 
                       (ScrollView) findViewById(R.id.tv_log).getParent() : null;

        surfaceView = findViewById(R.id.surface_view);
        tvNoVideo = findViewById(R.id.tv_no_video);
    }

    private void initializeRtspPlayer() {
        rtspPlayer = new RtspPlayer(this);
        rtspPlayer.setSurfaceView(surfaceView);
        rtspPlayer.setListener(new RtspPlayer.RtspPlayerListener() {
            @Override
            public void onStreamOpened(String streamInfo) {
                runOnUiThread(() -> {
                    logMessage("✅ RTSP流已连接");
                    logMessage("📄 流信息: " + streamInfo);
                    updateConnectionState(true);
                    updateDecoderInfo();
                    tvNoVideo.setVisibility(View.GONE);
                });
            }

            @Override
            public void onStreamClosed() {
                runOnUiThread(() -> {
                    logMessage("🔌 RTSP流已断开");
                    updateConnectionState(false);
                    resetPerformanceDisplay();
                    tvNoVideo.setVisibility(View.VISIBLE);
                    tvNoVideo.setText("等待视频连接...");
                });
            }

            @Override
            public void onRecordingStarted() {
                runOnUiThread(() -> {
                    logMessage("🎬 开始录制");
                    isRecording = true;
                    updateButtonStates();
                });
            }

            @Override
            public void onRecordingStopped() {
                runOnUiThread(() -> {
                    logMessage("⏹️ 录制已停止");
                    isRecording = false;
                    updateButtonStates();
                });
            }

            @Override
            public void onError(String error) {
                runOnUiThread(() -> {
                    logMessage("❌ 错误: " + error);
                    updateConnectionState(false);
                    resetPerformanceDisplay();
                    tvNoVideo.setVisibility(View.VISIBLE);
                    tvNoVideo.setText("连接错误");
                });
            }

            @Override
            public void onFrameProcessed() {
                // 更新性能统计
                runOnUiThread(() -> {
                    frameCount++;
                    if (performanceMonitor != null) {
                        performanceMonitor.updateFrameCount();
                    }
                });
            }

            @Override
            public void onVideoSizeChanged(int width, int height) {
                runOnUiThread(() -> {
                    // 根据视频尺寸调整 SurfaceView 的大小，保持宽高比
                    if (width > 0 && height > 0) {
                        float videoRatio = (float) width / height;
                        int surfaceWidth = surfaceView.getWidth();
                        int newHeight = (int) (surfaceWidth / videoRatio);
                        
                        android.view.ViewGroup.LayoutParams params = surfaceView.getLayoutParams();
                        params.height = newHeight;
                        surfaceView.setLayoutParams(params);
                    }
                });
            }
        });
        
        // 初始化性能监控器
        performanceMonitor = new PerformanceMonitor();
    }

    private void setupUI() {
        TextView tv = binding.sampleText;
        
        // 显示基本信息和FFmpeg版本
        String info = stringFromJNI() + "\n\n" + getFFmpegVersion();
        tv.setText(info);
        
        // 设置按钮点击事件
        btnConnect.setOnClickListener(v -> connectToRtsp());
        btnStartTest.setOnClickListener(v -> startLatencyTest());
        btnRecord.setOnClickListener(v -> toggleRecording());
        btnDisconnect.setOnClickListener(v -> disconnectRtsp());
        btnClearLog.setOnClickListener(v -> clearLog());
        
        // 设置硬件解码开关
        swHardwareDecode.setOnCheckedChangeListener((buttonView, isChecked) -> {
            setHardwareDecodeEnabled(isChecked);
            logMessage("🔧 硬件解码: " + (isChecked ? "启用" : "禁用"));
            updateDecoderInfo();
        });
        
        // 初始化UI状态
        updateButtonStates();
        updateDecoderInfo();
        logMessage("🚀 应用已启动，准备测试RTSP延迟");
    }

    // ==================== RTSP连接控制 ====================
    
    private void connectToRtsp() {
        final String rtspUrl = etRtspUrl.getText().toString().trim();
        if (rtspUrl.isEmpty()) {
            Toast.makeText(this, "请输入RTSP URL", Toast.LENGTH_SHORT).show();
            return;
        }
        
        logMessage("🔄 正在连接: " + rtspUrl);
        btnConnect.setEnabled(false);
        
        // 直接调用RtspPlayer的openStream方法，它会自动处理异步操作
        rtspPlayer.openStream(rtspUrl);
        btnConnect.setEnabled(true);
    }
    
    private void disconnectRtsp() {
        logMessage("🔄 正在断开连接...");
        
        executorService.execute(() -> {
            if (isRecording) {
                rtspPlayer.stopRecording();
            }
            if (isTesting) {
                stopLatencyTest();
            }
            rtspPlayer.closeStream();
        });
    }
    
    private void startLatencyTest() {
        if (!isConnected) {
            Toast.makeText(this, "请先连接RTSP流", Toast.LENGTH_SHORT).show();
            return;
        }
        
        isTesting = true;
        testStartTime = System.currentTimeMillis();
        frameCount = 0;
        lastFpsTime = testStartTime;
        
        // 重置性能统计
        resetPerformanceStats();
        
        // 刷新缓冲区以确保超低延迟
        flushBuffers();
        
        logMessage("⚡ 开始超低延迟测试");
        logMessage("🔄 性能统计已重置");
        logMessage("🚀 缓冲区已刷新，启动超低延迟模式");
        updateButtonStates();
        
        // 启动性能监控
        performanceMonitor.startMonitoring();
        
        // 开始优化的帧处理循环
        startOptimizedFrameProcessingLoop();
    }
    
    private void stopLatencyTest() {
        isTesting = false;
        performanceMonitor.stopMonitoring();
        
        final long finalTestStartTime = testStartTime;
        final int finalFrameCount = frameCount;
        
        runOnUiThread(() -> {
            logMessage("⏹️ 超低延迟测试已停止");
            updateButtonStates();
            
            // 显示测试结果摘要
            long totalTime = System.currentTimeMillis() - finalTestStartTime;
            float avgFps = finalFrameCount * 1000.0f / totalTime;
            
            String summary = String.format("📊 超低延迟测试摘要:\n总时长: %d秒\n总帧数: %d\n平均帧率: %.1f FPS", 
                    totalTime / 1000, finalFrameCount, avgFps);
            logMessage(summary);
            
            // 显示详细性能统计
            String nativeStats = getPerformanceStats();
            logMessage("📈 详细性能统计:\n" + nativeStats);
            
            // 获取native层的精确统计
            long avgDecodeTime = getAverageDecodeTime();
            int processedFrames = getProcessedFrameCount();
            
            if (processedFrames > 0) {
                logMessage(String.format("🎯 Native层统计: 已处理%d帧, 平均解码时间%dms", 
                        processedFrames, avgDecodeTime));
                
                // 评估延迟性能
                String performance;
                if (avgDecodeTime < 10) {
                    performance = "🟢 优秀 (超低延迟)";
                } else if (avgDecodeTime < 20) {
                    performance = "🟡 良好 (低延迟)";
                } else if (avgDecodeTime < 50) {
                    performance = "🟠 一般 (中等延迟)";
                } else {
                    performance = "🔴 较差 (高延迟)";
                }
                
                logMessage("🏆 延迟性能评级: " + performance);
            }
        });
    }
    
    private void startOptimizedFrameProcessingLoop() {
        executorService.execute(() -> {
            runOnUiThread(() -> logMessage("🎯 启动超低延迟帧处理循环"));
            
            long lastStatsTime = System.currentTimeMillis();
            int processedFrames = 0;
            int failedFrames = 0;
            long totalProcessTime = 0;
            
            while (isTesting && isConnected) {
                long frameStart = System.nanoTime();
                
                boolean success = rtspPlayer.processRtspFrame();
                
                long frameTime = (System.nanoTime() - frameStart) / 1_000_000; // 转换为毫秒
                totalProcessTime += frameTime;
                
                if (success) {
                    processedFrames++;
                    
                    // 更新性能数据
                    if (performanceMonitor != null) {
                        performanceMonitor.recordDecodeTime(frameTime);
                    }
                } else {
                    failedFrames++;
                    
                    // 连续失败处理
                    if (failedFrames > 10) {
                        runOnUiThread(() -> {
                            logMessage("❌ 连续帧处理失败，停止测试");
                            stopLatencyTest();
                        });
                        break;
                    }
                }
                
                // 每5秒输出一次详细统计
                long currentTime = System.currentTimeMillis();
                if (currentTime - lastStatsTime >= 5000) {
                    final int frames = processedFrames;
                    final int failed = failedFrames;
                    final long avgTime = frames > 0 ? totalProcessTime / frames : 0;
                    final float fps = frames * 1000.0f / (currentTime - lastStatsTime);
                    
                    runOnUiThread(() -> {
                        logMessage(String.format("📊 5秒统计: 处理%d帧, 失败%d帧, 平均耗时%dms, FPS=%.1f", 
                                frames, failed, avgTime, fps));
                    });
                    
                    // 重置计数器
                    lastStatsTime = currentTime;
                    processedFrames = 0;
                    failedFrames = 0;
                    totalProcessTime = 0;
                }
                
                // 超低延迟优化：动态调整处理间隔
                long sleepTime;
                if (frameTime < 3) {
                    sleepTime = 1;  // 处理很快，最小间隔
                } else if (frameTime < 10) {
                    sleepTime = 2;  // 处理中等，短间隔
                } else if (frameTime < 20) {
                    sleepTime = 5;  // 处理较慢，中等间隔
                } else {
                    sleepTime = 8;  // 处理很慢，较长间隔
                    
                    // 处理时间过长时刷新缓冲区
                    if (frameTime > 50) {
                        flushBuffers();
                        runOnUiThread(() -> logMessage("⚡ 检测到高延迟，已刷新缓冲区"));
                    }
                }
                
                try {
                    Thread.sleep(sleepTime);
                } catch (InterruptedException e) {
                    runOnUiThread(() -> logMessage("🔄 帧处理循环被中断"));
                    break;
                }
            }
            
            runOnUiThread(() -> logMessage("⏹️ 帧处理循环已结束"));
        });
    }
    
    private void toggleRecording() {
        if (!isConnected) {
            Toast.makeText(this, "请先连接RTSP流", Toast.LENGTH_SHORT).show();
            return;
        }
        
        if (isRecording) {
            // 停止录制
            rtspPlayer.stopRecording();
        } else {
            // 开始录制
            File storage = getExternalFilesDir(Environment.DIRECTORY_MOVIES);
            final String outputPath =  storage.getAbsolutePath() + "/record" +
                              "/rtsp_record_" + System.currentTimeMillis() + ".mp4";
            
            rtspPlayer.startRecording(outputPath);
        }
    }
    
    // ==================== UI状态更新 ====================
    
    private void updateConnectionState(boolean connected) {
        isConnected = connected;
        updateButtonStates();
        
        if (!connected) {
            isTesting = false;
            isRecording = false;
            if (performanceMonitor != null) {
                performanceMonitor.stopMonitoring();
            }
        }
    }
    
    private void updateButtonStates() {
        btnConnect.setEnabled(!isConnected);
        btnStartTest.setEnabled(isConnected && !isTesting);
        btnRecord.setEnabled(isConnected);
        btnDisconnect.setEnabled(isConnected);
        
        btnRecord.setText(isRecording ? "停止录制" : "开始录制");
        btnStartTest.setText(isTesting ? "测试中..." : "开始测试");
    }
    
    private void updateDecoderInfo() {
        String decoderInfo = getDecoderInfo();
        tvDecoderInfo.setText("解码器: " + decoderInfo);
    }
    
    private void resetPerformanceDisplay() {
        tvDecodeLatency.setText("-- ms");
        tvFps.setText("-- FPS");
        tvNetworkLatency.setText("-- ms");
        tvTotalLatency.setText("-- ms");
    }
    
    private void clearLog() {
        tvLog.setText("日志已清除\n");
        scrollLogToBottom();
    }
    
    private void logMessage(String message) {
        String timestamp = DateFormat.format("HH:mm:ss", new Date()).toString();
        String logEntry = "[" + timestamp + "] " + message + "\n";
        
        String currentLog = tvLog.getText().toString();
        tvLog.setText(currentLog + logEntry);
        
        // 限制日志长度
        if (currentLog.length() > 10000) {
            String[] lines = currentLog.split("\n");
            StringBuilder trimmedLog = new StringBuilder();
            for (int i = Math.max(0, lines.length - 100); i < lines.length; i++) {
                trimmedLog.append(lines[i]).append("\n");
            }
            tvLog.setText(trimmedLog.toString() + logEntry);
        }
        
        scrollLogToBottom();
        Log.d(TAG, message);
    }
    
    private void scrollLogToBottom() {
        if (logScrollView != null) {
            logScrollView.post(() -> logScrollView.fullScroll(View.FOCUS_DOWN));
        }
    }
    
    // 移除Activity生命周期绑定 - 改为纯Surface状态管理

    @Override
    protected void onDestroy() {
        super.onDestroy();
        
        // 清理资源
        if (rtspPlayer != null) {
            if (isRecording) {
                rtspPlayer.stopRecording();
            }
            if (isConnected) {
                rtspPlayer.closeStream();
            }
        }
        
        if (performanceMonitor != null) {
            performanceMonitor.stopMonitoring();
        }
        
        if (executorService != null && !executorService.isShutdown()) {
            executorService.shutdown();
        }
        
        // 最终清理Surface
        setSurface(null);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == PERMISSION_REQUEST_CODE) {
            if (grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
                Toast.makeText(this, "权限已获取", Toast.LENGTH_SHORT).show();
            } else {
                Toast.makeText(this, "需要存储权限来访问视频文件", Toast.LENGTH_LONG).show();
            }
        }
    }
    
    // ==================== 性能监控器类 ====================
    
    private class PerformanceMonitor {
        private boolean isMonitoring = false;
        private Handler updateHandler;
        private Runnable updateRunnable;
        
        // 性能数据
        private long totalDecodeTime = 0;
        private int decodeCount = 0;
        private long lastUpdateTime = 0;
        private int lastFrameCount = 0;
        
        // 移动平均计算
        private final int WINDOW_SIZE = 30;
        private long[] decodeTimeWindow = new long[WINDOW_SIZE];
        private int windowIndex = 0;
        
        public void startMonitoring() {
            if (isMonitoring) return;
            
            isMonitoring = true;
            updateHandler = new Handler(Looper.getMainLooper());
            lastUpdateTime = System.currentTimeMillis();
            lastFrameCount = frameCount;
            
            updateRunnable = new Runnable() {
                @Override
                public void run() {
                    if (isMonitoring) {
                        updatePerformanceDisplay();
                        updateHandler.postDelayed(this, 1000); // 每秒更新
                    }
                }
            };
            
            updateHandler.post(updateRunnable);
        }
        
        public void stopMonitoring() {
            isMonitoring = false;
            if (updateHandler != null && updateRunnable != null) {
                updateHandler.removeCallbacks(updateRunnable);
            }
        }
        
        public void recordDecodeTime(long decodeTime) {
            totalDecodeTime += decodeTime;
            decodeCount++;
            
            // 更新移动窗口
            decodeTimeWindow[windowIndex] = decodeTime;
            windowIndex = (windowIndex + 1) % WINDOW_SIZE;
        }
        
        public void updateFrameCount() {
            // 这个方法会在帧处理回调中被调用
        }
        
        private void updatePerformanceDisplay() {
            final long currentTime = System.currentTimeMillis();
            final long timeDiff = currentTime - lastUpdateTime;
            final int frameDiff = frameCount - lastFrameCount;
            
            // 计算FPS（基于Java层统计）
            final float fps = (timeDiff > 0) ? frameDiff * 1000.0f / timeDiff : 0;
            
            // 从native层获取真实的解码延迟
            final long nativeAvgDecodeTime = getAverageDecodeTime();
            final int nativeFrameCount = getProcessedFrameCount();
            
            // 使用native层的数据（如果可用），否则使用Java层的估算
            final long actualDecodeLatency = nativeAvgDecodeTime > 0 ? nativeAvgDecodeTime : calculateMovingAverage();
            
            // 模拟网络延迟（实际项目中可以通过ping测试获得）
            final long networkLatency = simulateNetworkLatency();
            
            // 计算总延迟
            final long totalLatency = actualDecodeLatency + networkLatency;
            
            // 更新UI
            runOnUiThread(() -> {
                tvFps.setText(String.format("%.1f FPS", fps));
                tvDecodeLatency.setText(String.format("%d ms", actualDecodeLatency));
                tvNetworkLatency.setText(String.format("%d ms", networkLatency));
                tvTotalLatency.setText(String.format("%d ms", totalLatency));
                
                // 根据延迟设置颜色
                int color;
                if (totalLatency < 100) {
                    color = getResources().getColor(android.R.color.holo_green_dark);
                } else if (totalLatency < 200) {
                    color = getResources().getColor(android.R.color.holo_orange_dark);
                } else {
                    color = getResources().getColor(android.R.color.holo_red_dark);
                }
                tvTotalLatency.setTextColor(color);
                
                // 记录详细性能信息到日志
                if (nativeFrameCount > 0 && nativeFrameCount % 100 == 0) { // 每100帧记录一次
                    // 获取丢帧统计
                    String stats = getPerformanceStats();
                    logMessage(String.format("📊 实时性能: FPS=%.1f, 解码=%dms, 网络=%dms, 总计=%dms", 
                        fps, actualDecodeLatency, networkLatency, totalLatency));
                    logMessage("📈 " + stats);
                }
            });
            
            // 更新记录
            lastUpdateTime = currentTime;
            lastFrameCount = frameCount;
        }
        
        private long calculateMovingAverage() {
            long sum = 0;
            int count = 0;
            for (long time : decodeTimeWindow) {
                if (time > 0) {
                    sum += time;
                    count++;
                }
            }
            return count > 0 ? sum / count : 0;
        }
        
        private long simulateNetworkLatency() {
            // 模拟网络延迟，实际应用中可以通过ping测试或RTP时间戳计算
            // 这里返回一个模拟值，范围在20-80ms之间
            return 20 + (System.currentTimeMillis() % 60);
        }
    }

    /**
     * A native method that is implemented by the 'CompileFfmpeg' native library,
     * which is packaged with this application.
     */
    public native String stringFromJNI();
    
    /**
     * 获取FFmpeg版本信息
     */
    public native String getFFmpegVersion();
    
    /**
     * 获取视频文件信息
     */
    public native String getVideoInfo(String path);
    
    /**
     * 转换视频文件
     */
    public native boolean convertVideo(String inputPath, String outputPath);
    
    // RTSP相关的native方法
    /**
     * 打开RTSP视频流
     * @param rtspUrl RTSP URL地址
     * @return 是否成功打开
     */
    public native boolean openRtspStream(String rtspUrl);
    
    /**
     * 获取RTSP流信息
     * @return RTSP流的详细信息
     */
    public native String getRtspStreamInfo();
    
    /**
     * 开始录制RTSP流
     * @param outputPath 输出文件路径
     * @return 是否成功开始录制
     */
    public native boolean startRtspRecording(String outputPath);
    
    /**
     * 停止录制RTSP流
     * @return 是否成功停止录制
     */
    public native boolean stopRtspRecording();
    
    /**
     * 处理RTSP帧数据（需要循环调用）
     * @return 是否成功处理帧数据
     */
    public native boolean processRtspFrame();
    
    /**
     * 关闭RTSP流
     */
    public native void closeRtspStream();
    
    // 硬件解码控制相关的native方法
    /**
     * 设置硬件解码开关
     * @param enabled true启用硬件解码，false禁用
     */
    public native void setHardwareDecodeEnabled(boolean enabled);
    
    /**
     * 查询硬件解码开关状态
     * @return true表示启用，false表示禁用
     */
    public native boolean isHardwareDecodeEnabled();
    
    /**
     * 查询当前是否正在使用硬件解码
     * @return true表示正在使用硬件解码，false表示使用软件解码
     */
    public native boolean isHardwareDecodeAvailable();
    
    /**
     * 获取解码器详细信息
     * @return 包含当前解码器状态和支持的硬件解码类型的详细信息
     */
    public native String getDecoderInfo();
    
    // 性能监控相关的native方法
    /**
     * 获取性能统计信息
     * @return 包含帧数、解码时间等性能数据的详细信息
     */
    public native String getPerformanceStats();
    
    /**
     * 重置性能统计数据
     */
    public native void resetPerformanceStats();
    
    /**
     * 获取平均解码时间
     * @return 平均解码时间（毫秒）
     */
    public native long getAverageDecodeTime();
    
    /**
     * 获取已处理的帧数
     * @return 已处理的帧数
     */
    public native int getProcessedFrameCount();

    /**
     * 刷新解码器缓冲区，用于网络抖动后快速恢复超低延迟
     */
    public native void flushBuffers();
    
    /**
     * 设置视频输出的Surface
     * @param surface Surface对象，用于显示视频
     */
    public native void setSurface(Surface surface);
    
    // 移除Activity生命周期native方法 - 改为纯Surface状态管理

}