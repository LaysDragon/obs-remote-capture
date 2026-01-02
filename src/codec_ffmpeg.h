/*
 * codec_ffmpeg.h
 * FFmpeg H.264 硬體編解碼器
 * 
 * 支援的編碼器 (依優先順序):
 * - h264_nvenc (NVIDIA)
 * - h264_qsv (Intel Quick Sync)
 * - h264_amf (AMD)
 * - libx264 (軟體 fallback)
 */

#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

// ========== 編碼器介面 ==========
class FFmpegEncoder {
public:
    FFmpegEncoder();
    ~FFmpegEncoder();
    
    // 初始化編碼器 (自動選擇最佳硬體編碼器)
    bool init(uint32_t width, uint32_t height, int fps = 30, int bitrate_kbps = 8000);
    
    // 編碼一幀 BGRA 數據
    // 返回 H.264 NAL units
    bool encode(const uint8_t* bgra_data, uint32_t width, uint32_t height, 
                uint32_t linesize, std::vector<uint8_t>& out_data,
                bool& is_keyframe);
    
    // 獲取編碼器名稱
    const char* getName() const;
    
    // 獲取 SPS/PPS (用於解碼器初始化)
    const std::vector<uint8_t>& getExtraData() const { return extra_data_; }
    
    // 重置編碼器 (尺寸變化時)
    void reset();

private:
    bool initEncoder(const char* encoder_name);
    
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    SwsContext* sws_ = nullptr;
    
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    int64_t pts_ = 0;
    
    std::vector<uint8_t> extra_data_;
    std::string encoder_name_;
};

// ========== 解碼器介面 ==========
class FFmpegDecoder {
public:
    FFmpegDecoder();
    ~FFmpegDecoder();
    
    // 初始化解碼器
    bool init(const uint8_t* extra_data = nullptr, size_t extra_size = 0);
    
    // 解碼 H.264 數據到 BGRA
    bool decode(const uint8_t* h264_data, size_t size,
                uint32_t expected_width, uint32_t expected_height,
                std::vector<uint8_t>& out_bgra);
    
    // 獲取解碼器名稱
    const char* getName() const;
    
    // 重置解碼器
    void reset();

private:
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    SwsContext* sws_ = nullptr;
    
    uint32_t last_width_ = 0;
    uint32_t last_height_ = 0;
    
    std::string decoder_name_;
};
