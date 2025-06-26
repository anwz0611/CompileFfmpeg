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
        Log.d(TAG, "🔄 surfaceCreated: " + holder.getSurface());
        this.surface = holder.getSurface();
        
        // 延迟设置Surface，确保系统完全准备就绪
        if (mainActivity != null && surface != null && surface.isValid()) {
            // 使用Handler延迟50ms设置，避免Surface状态不稳定
            new Handler(Looper.getMainLooper()).postDelayed(() -> {
                if (surface != null && surface.isValid()) {
                    mainActivity.setSurface(surface);
                    Log.d(TAG, "✅ Surface延迟设置完成");
                }
            }, 50);
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        Log.d(TAG, "🔄 surfaceChanged: format=" + format + ", size=" + width + "x" + height);
        Surface newSurface = holder.getSurface();
        
        // 只有当Surface真正改变时才重新设置
        if (newSurface != this.surface) {
            this.surface = newSurface;
            if (mainActivity != null && surface != null && surface.isValid()) {
                mainActivity.setSurface(surface);
            }
        } else {
            Log.d(TAG, "🔄 Surface未改变，跳过重复设置");
        }
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        Log.d(TAG, "🔄 surfaceDestroyed: 立即清理Surface");
        
        // 立即清理Surface，确保Native层停止渲染
        if (mainActivity != null) {
            mainActivity.setSurface(null);
        }
        this.surface = null;
        
        Log.d(TAG, "✅ Surface销毁完成");
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
        Log.i(TAG, "打开RTSP流: " + rtspUrl);
        
        new AsyncTask<String, Void, Boolean>() {
            @Override
            protected Boolean doInBackground(String... urls) {
                return mainActivity != null ? mainActivity.openRtspStream(urls[0]) : false;
            }
            
            @Override
            protected void onPostExecute(Boolean success) {
                if (success) {
                    String streamInfo = mainActivity != null ? mainActivity.getRtspStreamInfo() : "未知";
                    Log.i(TAG, "RTSP流打开成功: " + streamInfo);
                    
                    isPlaying = true;
                    startFrameProcessing();
                    
                    if (listener != null) {
                        listener.onStreamOpened(streamInfo);
                    }
                } else {
                    Log.e(TAG, "RTSP流打开失败");
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
        if (!isPlaying) {
            Log.e(TAG, "RTSP流未打开，无法开始录制");
            if (listener != null) {
                listener.onError("RTSP流未打开，无法开始录制");
            }
            return;
        }
        
        Log.i(TAG, "开始录制到: " + outputPath);
        
        new AsyncTask<String, Void, Boolean>() {
            @Override
            protected Boolean doInBackground(String... paths) {
                return mainActivity != null ? mainActivity.startRtspRecording(paths[0]) : false;
            }
            
            @Override
            protected void onPostExecute(Boolean success) {
                if (success) {
                    isRecording = true;
                    Log.i(TAG, "录制开始成功");
                    if (listener != null) {
                        listener.onRecordingStarted();
                    }
                } else {
                    Log.e(TAG, "录制开始失败");
                    if (listener != null) {
                        listener.onError("录制开始失败");
                    }
                }
            }
        }.execute(outputPath);
    }
    
    /**
     * 停止录制
     */
    public void stopRecording() {
        if (!isRecording) {
            Log.w(TAG, "录制未在进行中");
            return;
        }
        
        Log.i(TAG, "停止录制");
        
        new AsyncTask<Void, Void, Boolean>() {
            @Override
            protected Boolean doInBackground(Void... voids) {
                return mainActivity != null ? mainActivity.stopRtspRecording() : false;
            }
            
            @Override
            protected void onPostExecute(Boolean success) {
                if (success) {
                    isRecording = false;
                    Log.i(TAG, "录制停止成功");
                    if (listener != null) {
                        listener.onRecordingStopped();
                    }
                } else {
                    Log.e(TAG, "录制停止失败");
                    if (listener != null) {
                        listener.onError("录制停止失败");
                    }
                }
            }
        }.execute();
    }
    
    /**
     * 关闭RTSP流
     */
    public void closeStream() {
        Log.i(TAG, "关闭RTSP流");
        
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
                Log.i(TAG, "RTSP流关闭完成");
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
            Log.i(TAG, "开始帧处理循环");
            
            while (isPlaying && !isCancelled()) {
                try {
                    boolean success = mainActivity != null ? mainActivity.processRtspFrame() : false;
                    
                    if (success) {
                        // 通知主线程处理了一帧
                        mainHandler.post(() -> {
                            if (listener != null) {
                                listener.onFrameProcessed();
                            }
                        });
                        
                        // 超低延迟模式：最小休眠时间
                        Thread.sleep(5); // 最小延迟，让CPU有时间处理其他任务
                    } else {
                        // 处理失败，可能是流结束或出错
                        Log.w(TAG, "帧处理失败，可能是流结束");
                        break;
                    }
                } catch (InterruptedException e) {
                    Log.i(TAG, "帧处理线程被中断");
                    break;
                } catch (Exception e) {
                    Log.e(TAG, "帧处理异常: " + e.getMessage());
                    break;
                }
            }
            
            Log.i(TAG, "帧处理循环结束");
            return null;
        }
        
        @Override
        protected void onPostExecute(Void aVoid) {
            // 帧处理结束，可能需要通知UI
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
} 