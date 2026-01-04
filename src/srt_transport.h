/*
 * srt_transport.h
 * SRT 串流傳輸層封裝
 *
 * 功能:
 * 1. SrtServer - 伺服器端，發送 MPEG-TS over SRT
 * 2. SrtClient - 客戶端，接收 MPEG-TS over SRT
 *
 * 使用 FFmpeg 的 SRT muxer/demuxer (libavformat)
 */

#pragma once

#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <thread>
#include <cstdint>
#include <vector>

// ========== SRT 連接資訊 ==========
struct SrtConnectionInfo {
    int port = 0;
    int latency_ms = 200;
    std::string passphrase;
    bool ready = false;
};

// ========== SRT Server (發送端) ==========
class SrtServer {
public:
    SrtServer();
    ~SrtServer();
    
    // 禁止複製
    SrtServer(const SrtServer&) = delete;
    SrtServer& operator=(const SrtServer&) = delete;
    
    // 啟動 SRT 監聽
    // @param port 監聽端口
    // @param latency_ms SRT 延遲設定 (影響緩衝大小)
    // @param width 視訊寬度 (用於編碼器配置)
    // @param height 視訊高度
    // @param fps 幀率
    bool start(int port, int latency_ms = 200, 
               uint32_t width = 1920, uint32_t height = 1080, int fps = 30);
    
    // 停止 SRT 服務
    void stop();
    
    // 服務是否運行中
    bool isRunning() const;
    
    // 是否有客戶端連接
    bool hasClient() const;
    
    // 發送視訊幀 (已編碼的 H.264 NAL units)
    // @param data H.264 編碼數據
    // @param size 數據大小
    // @param pts_us 顯示時間戳 (微秒)
    // @param is_keyframe 是否為關鍵幀
    bool sendVideoFrame(const uint8_t* data, size_t size, 
                        int64_t pts_us, bool is_keyframe = false);
    
    // 發送音訊幀 (PCM Float Planar)
    // @param data 音訊數據 (planar float, L|R)
    // @param samples 每聲道樣本數
    // @param sample_rate 採樣率
    // @param channels 聲道數
    // @param pts_us 顯示時間戳 (微秒)
    bool sendAudioFrame(const float* data, size_t samples,
                        uint32_t sample_rate, uint32_t channels, 
                        int64_t pts_us);
    
    // 獲取連接資訊
    SrtConnectionInfo getInfo() const;
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// ========== SRT Client (接收端) ==========
class SrtClient {
public:
    // 回調類型定義
    using VideoCallback = std::function<void(
        const uint8_t* h264_data, size_t size,
        int64_t pts_us, bool is_keyframe)>;
    
    using AudioCallback = std::function<void(
        const float* pcm_data, size_t samples,
        uint32_t sample_rate, uint32_t channels,
        int64_t pts_us)>;
    
    SrtClient();
    ~SrtClient();
    
    // 禁止複製
    SrtClient(const SrtClient&) = delete;
    SrtClient& operator=(const SrtClient&) = delete;
    
    // 連接到 SRT Server
    // @param host 伺服器地址
    // @param port 伺服器端口
    // @param latency_ms SRT 延遲設定
    bool connect(const std::string& host, int port, int latency_ms = 200);
    
    // 斷開連接
    void disconnect();
    
    // 是否已連接
    bool isConnected() const;
    
    // 開始接收串流 (會啟動接收線程)
    // @param on_video 視訊回調
    // @param on_audio 音訊回調
    bool startReceive(VideoCallback on_video, AudioCallback on_audio);
    
    // 停止接收
    void stopReceive();
    
    // 是否正在接收
    bool isReceiving() const;
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
