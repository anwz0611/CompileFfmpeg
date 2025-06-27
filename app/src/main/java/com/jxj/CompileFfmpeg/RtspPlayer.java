package com.jxj.CompileFfmpeg;

import android.os.AsyncTask;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

/**
 * RTSP播放器类 - 提供低延迟RTSP流播放和录制功能
 */
public class RtspPlayer implements SurfaceHolder.Callback {
    private static final String TAG = "RtspPlayer";
    
    // 通过MainActivity访问native方法
    private MainActivity mainActivity;
    
    private Handler mainHandler;
    private boolean isPlaying = false;
    private boolean isRecording = false;
    private FrameProcessTask frameTask;
    private Surface surface;
    private int videoWidth = 0;
    private int videoHeight = 0;
    
    // 回调接口
    public interface RtspPlayerListener {
        void onStreamOpened(String streamInfo);
        void onStreamClosed();
        void onRecordingStarted();
        void onRecordingStopped();
        void onError(String error);
        void onFrameProcessed(); // 每处理一帧时回调
        void onVideoSizeChanged(int width, int height); // 视频尺寸变化时回调
    }
    
    private RtspPlayerListener listener;
    
    public RtspPlayer() {
        mainHandler = new Handler(Looper.getMainLooper());
    }
    
    public RtspPlayer(MainActivity activity) {
        this.mainActivity = activity;
        mainHandler = new Handler(Looper.getMainLooper());
    }
    
    public void setListener(RtspPlayerListener listener) {
        this.listener = listener;
    }

    public void setSurfaceView(SurfaceView surfaceView) {
        if (surfaceView != null) {
            SurfaceHolder holder = surfaceView.getHolder();
            holder.addCallback(this);
            // 设置surface类型
            holder.setType(SurfaceHolder.SURFACE_TYPE_PUSH_BUFFERS);
        }
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        this.surface = holder.getSurface();
        
        if (mainActivity != null && surface != null && surface.isValid()) {
            new Handler(Looper.getMainLooper()).postDelayed(() -> {
                if (surface != null && surface.isValid()) {
                    mainActivity.setSurface(surface);
                }
            }, 50);
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        Surface newSurface = holder.getSurface();
        
        if (newSurface != this.surface) {
            this.surface = newSurface;
            if (mainActivity != null && surface != null && surface.isValid()) {
                mainActivity.setSurface(surface);
            }
        }
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        if (mainActivity != null) {
            mainActivity.setSurface(null);
        }
        this.surface = null;
    }

    public void setVideoSize(int width, int height) {
        if (width != videoWidth || height != videoHeight) {
            videoWidth = width;
            videoHeight = height;
            if (listener != null) {
                listener.onVideoSizeChanged(width, height);
            }
        }
    }
    
    /**
     * 打开RTSP流（支持低延迟配置）
     */
    public void openStream(String rtspUrl) {
        new AsyncTask<String, Void, Boolean>() {
            @Override
            protected Boolean doInBackground(String... urls) {
                return mainActivity != null ? mainActivity.openRtspStream(urls[0]) : false;
            }
            
            @Override
            protected void onPostExecute(Boolean success) {
                if (success) {
                    String streamInfo = mainActivity != null ? mainActivity.getRtspStreamInfo() : "未知";
                    
                    isPlaying = true;
                    startFrameProcessing();
                    
                    if (listener != null) {
                        listener.onStreamOpened(streamInfo);
                    }
                } else {
                    if (listener != null) {
                        listener.onError("无法打开RTSP流: " + rtspUrl);
                    }
                }
            }
        }.execute(rtspUrl);
    }
    
    /**
     * 开始录制
     */
    public void startRecording(String outputPath) {
        if (!isPlaying || isRecording) {
            return;
        }
        
        // 创建录制目录和MP4文件
        java.io.File file = new java.io.File(outputPath);
        java.io.File parentDir = file.getParentFile();
        
        if (parentDir != null && !parentDir.exists()) {
            parentDir.mkdirs();
        }
        
        // 简化文件创建，让Native层处理
        try {
            if (file.exists()) {
                file.delete();
            }
            
            // 只确保目录存在，不预创建文件
            // Native层会创建和管理文件
            
        } catch (Exception e) {
            Log.e(TAG, "文件准备失败: " + e.getMessage());
            return;
        }
        
        new Thread(() -> {
            try {
                Log.i(TAG, "🔧 开始录制线程启动");
                boolean success = false;
                // 先准备录制环境
                if (mainActivity != null) {
                    Log.i(TAG, "🔧 调用prepareRecording");
                    boolean prepared = mainActivity.prepareRecording(outputPath);
                    Log.i(TAG, "🔧 prepareRecording结果: " + prepared);
                    
                    if (prepared) {
                        Log.i(TAG, "🔧 调用startRtspRecording");
                        // 启动录制
                        boolean started = mainActivity.startRtspRecording(outputPath);
                        Log.i(TAG, "🔧 startRtspRecording结果: " + started);
                        if (started) {
                            success = true;
                        }
                    }
                } else {
                    Log.e(TAG, "🔧 mainActivity为空");
                }
                
                Log.i(TAG, "🔧 录制准备完成，结果: " + success);
                
                // 在主线程更新UI
                final boolean finalSuccess = success;
                mainHandler.post(() -> {
                    if (finalSuccess) {
                        Log.i(TAG, "🔧 录制启动成功，更新状态");
                        isRecording = true;
                        if (listener != null) {
                            listener.onRecordingStarted();
                        }
                    } else {
                        Log.e(TAG, "🔧 录制启动失败");
                        if (listener != null) {
                            listener.onError("录制开始失败");
                        }
                    }
                });
                
            } catch (Exception e) {
                Log.e(TAG, "🔧 录制启动异常: " + e.getMessage(), e);
                // 在主线程更新UI
                mainHandler.post(() -> {
                    if (listener != null) {
                        listener.onError("录制启动异常: " + e.getMessage());
                    }
                });
            }
        }).start();
    }
    
    public void stopRecording() {
        Log.d(TAG, "🔧 stopRecording 被调用，当前状态: " + isRecording);
        
        // 使用Thread代替AsyncTask，更可靠
        new Thread(() -> {
            try {
                Log.d(TAG, "🔧 停止录制线程启动");
                Log.d(TAG, "🔧 调用 native stopRtspRecording");
                
                boolean result = false;
                if (mainActivity != null) {
                    result = mainActivity.stopRtspRecording();
                    Log.d(TAG, "🔧 native stopRtspRecording 结果: " + result);
                } else {
                    Log.e(TAG, "🔧 mainActivity 为空");
                }
                
                // 在主线程更新UI
                final boolean finalResult = result;
                mainHandler.post(() -> {
                    Log.d(TAG, "🔧 停止录制完成，结果: " + finalResult);
                    isRecording = false; // 强制设置为false
                    
                    if (finalResult) {
                        if (listener != null) {
                            listener.onRecordingStopped();
                        }
                    } else {
                        if (listener != null) {
                            listener.onError("录制停止失败，但已强制停止");
                        }
                    }
                });
                
            } catch (Exception e) {
                Log.e(TAG, "🔧 stopRtspRecording 异常: " + e.getMessage(), e);
                // 在主线程更新UI
                mainHandler.post(() -> {
                    isRecording = false; // 强制设置为false
                    if (listener != null) {
                        listener.onError("录制停止异常: " + e.getMessage());
                    }
                });
            }
        }).start();
    }
    
    /**
     * 关闭RTSP流
     */
    public void closeStream() {
        isPlaying = false;
        stopFrameProcessing();
        
        if (isRecording) {
            stopRecording();
        }
        
        new AsyncTask<Void, Void, Void>() {
            @Override
            protected Void doInBackground(Void... voids) {
                if (mainActivity != null) {
                    mainActivity.closeRtspStream();
                }
                return null;
            }
            
            @Override
            protected void onPostExecute(Void aVoid) {
                if (listener != null) {
                    listener.onStreamClosed();
                }
            }
        }.execute();
    }
    
    /**
     * 开始帧处理循环
     */
    private void startFrameProcessing() {
        if (frameTask != null) {
            frameTask.cancel(true);
        }
        
        frameTask = new FrameProcessTask();
        frameTask.execute();
    }
    
    /**
     * 停止帧处理循环
     */
    private void stopFrameProcessing() {
        if (frameTask != null) {
            frameTask.cancel(true);
            frameTask = null;
        }
    }
    
    /**
     * 帧处理异步任务
     */
    private class FrameProcessTask extends AsyncTask<Void, Void, Void> {
        @Override
        protected Void doInBackground(Void... voids) {
            while (isPlaying && !isCancelled()) {
                try {
                    boolean success = mainActivity != null ? mainActivity.processRtspFrame() : false;
                    
                    if (success) {
                        mainHandler.post(() -> {
                            if (listener != null) {
                                listener.onFrameProcessed();
                            }
                        });
                        
                        Thread.sleep(5);
                    } else {
                        break;
                    }
                } catch (InterruptedException e) {
                    break;
                } catch (Exception e) {
                    break;
                }
            }
            
            return null;
        }
        
        @Override
        protected void onPostExecute(Void aVoid) {
            if (listener != null && isPlaying) {
                listener.onError("RTSP流处理结束");
            }
        }
    }
    
    // 获取播放状态
    public boolean isPlaying() {
        return isPlaying;
    }
    
    // 获取录制状态
    public boolean isRecording() {
        return isRecording;
    }
    
    // 获取流信息
    public String getStreamInfo() {
        if (isPlaying && mainActivity != null) {
            return mainActivity.getRtspStreamInfo();
        }
        return "RTSP流未打开";
    }
    
    // 提供给MainActivity调用的方法
    public boolean processRtspFrame() {
        return mainActivity != null ? mainActivity.processRtspFrame() : false;
    }
    
    private boolean writeBasicMp4Header(java.io.File file) {
        try (java.io.FileOutputStream fos = new java.io.FileOutputStream(file)) {
            writeFtypBox(fos);
            writeMdatBoxHeader(fos);
            fos.flush();
            return true;
        } catch (java.io.IOException e) {
            return false;
        }
    }
    
    private void writeFtypBox(java.io.FileOutputStream fos) throws java.io.IOException {
        writeUInt32BE(fos, 32);           // box大小
        writeUInt32BE(fos, 0x66747970);   // 'ftyp'
        writeUInt32BE(fos, 0x69736F6D);   // 'isom'
        writeUInt32BE(fos, 0x00000200);   // version
        writeUInt32BE(fos, 0x69736F6D);   // 'isom'
        writeUInt32BE(fos, 0x69736F32);   // 'iso2'
        writeUInt32BE(fos, 0x61766331);   // 'avc1'
        writeUInt32BE(fos, 0x6D703431);   // 'mp41'
    }
    
    private void writeMdatBoxHeader(java.io.FileOutputStream fos) throws java.io.IOException {
        writeUInt32BE(fos, 8);            // 临时大小
        writeUInt32BE(fos, 0x6D646174);   // 'mdat'
    }
    
    private void writeUInt32BE(java.io.FileOutputStream fos, int value) throws java.io.IOException {
        byte[] bytes = new byte[4];
        bytes[0] = (byte) ((value >> 24) & 0xFF);
        bytes[1] = (byte) ((value >> 16) & 0xFF);
        bytes[2] = (byte) ((value >> 8) & 0xFF);
        bytes[3] = (byte) (value & 0xFF);
        fos.write(bytes);
    }
} 