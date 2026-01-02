/*
 * codec_ffmpeg.cpp
 * FFmpeg H.264 硬體編解碼器實現
 */

#include "codec_ffmpeg.h"
#include <obs-module.h>
#include <cstring>



// ========== 輔助功能 ==========
extern "C" void obs_ffmpeg_log_available_encoders(void) {
    blog(LOG_INFO, "[FFmpeg] --- Available Encoders ---");
    
    void* iter = nullptr;
    const AVCodec* codec = nullptr;
    
    while ((codec = av_codec_iterate(&iter))) {
        if (av_codec_is_encoder(codec)) {
            // 只列出視頻編碼器，或者包含 H.264 的
            if (codec->type == AVMEDIA_TYPE_VIDEO) {
                 // 檢查是否為 H.264 相關 (可選，這裡列出所有視頻編碼器以便調試)
                //  if (strstr(codec->name, "h264") || strstr(codec->name, "qsv") || strstr(codec->name, "nvenc")) {
                //      blog(LOG_INFO, "[FFmpeg] Encoder: %s (%s)", codec->name, codec->long_name);
                //  }
                blog(LOG_INFO, "[FFmpeg] Encoder: %s (%s)", codec->name, codec->long_name);

            }
        }
    }
    blog(LOG_INFO, "[FFmpeg] --------------------------");
}

// ========== 編碼器實現 ==========

FFmpegEncoder::FFmpegEncoder() = default;

FFmpegEncoder::~FFmpegEncoder() {
    reset();
}

void FFmpegEncoder::reset() {
    if (sws_) {
        sws_freeContext(sws_);
        sws_ = nullptr;
    }
    if (pkt_) {
        av_packet_free(&pkt_);
    }
    if (frame_) {
        av_frame_free(&frame_);
    }
    if (ctx_) {
        avcodec_free_context(&ctx_);
    }
    width_ = height_ = 0;
    pts_ = 0;
}

bool FFmpegEncoder::initEncoder(const char* encoder_name) {
    blog(LOG_INFO, "[FFmpeg] Trying encoder: %s", encoder_name);
    const AVCodec* codec = avcodec_find_encoder_by_name(encoder_name);
    if (!codec) {
        blog(LOG_WARNING, "[FFmpeg] Encoder not found: %s", encoder_name);
        return false;
    }
    
    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) {
        blog(LOG_ERROR, "[FFmpeg] Failed to allocate encoder context");
        return false;
    }
    
    ctx_->width = width_;
    ctx_->height = height_;
    ctx_->time_base = {1, 1000};  // 毫秒為單位
    ctx_->framerate = {30, 1};
    ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx_->bit_rate = 8000000;  // 8 Mbps
    ctx_->gop_size = 30;  // 每 30 幀一個 keyframe
    ctx_->max_b_frames = 0;  // 禁用 B-frame 減少延遲
    
    // 編碼器特定設定
    if (strstr(encoder_name, "nvenc")) {
        // NVENC 設定
        av_opt_set(ctx_->priv_data, "preset", "p1", 0);  // 最快
        av_opt_set(ctx_->priv_data, "tune", "ll", 0);    // 低延遲
        av_opt_set(ctx_->priv_data, "rc", "cbr", 0);
        av_opt_set(ctx_->priv_data, "delay", "0", 0);
        av_opt_set(ctx_->priv_data, "zerolatency", "1", 0);
    } else if (strstr(encoder_name, "qsv")) {
        // Intel QSV 設定
        av_opt_set(ctx_->priv_data, "preset", "veryfast", 0);
        av_opt_set(ctx_->priv_data, "low_power", "1", 0);
        // QSV 需要不同的像素格式
        ctx_->pix_fmt = AV_PIX_FMT_NV12;
    } else if (strstr(encoder_name, "amf")) {
        // AMD AMF 設定
        av_opt_set(ctx_->priv_data, "quality", "speed", 0);
        av_opt_set(ctx_->priv_data, "rc", "cbr", 0);
    } else if (strstr(encoder_name, "libx264")) {
        // 軟體編碼器設定
        av_opt_set(ctx_->priv_data, "preset", "ultrafast", 0);
        av_opt_set(ctx_->priv_data, "tune", "zerolatency", 0);
    }
    
    int ret = avcodec_open2(ctx_, codec, nullptr);
    if (ret < 0) {
        char err_buf[256];
        av_strerror(ret, err_buf, sizeof(err_buf));
        blog(LOG_WARNING, "[FFmpeg] Failed to open encoder %s: %s (error code: %d)", 
             encoder_name, err_buf, ret);
        avcodec_free_context(&ctx_);
        return false;
    }
    
    
    encoder_name_ = encoder_name;
    return true;
}

bool FFmpegEncoder::init(uint32_t width, uint32_t height, int fps, int bitrate_kbps) {
    reset();
    
    width_ = width;
    height_ = height;
    
    // 嘗試各種編碼器
    const char* encoders[] = {
        "h264_nvenc",   // NVIDIA
        "h264_qsv",     // Intel
        "h264_amf",     // AMD
        "libx264",      // 軟體 fallback
        nullptr
    };
    
    for (int i = 0; encoders[i]; i++) {
        if (initEncoder(encoders[i])) {
            blog(LOG_INFO, "[FFmpeg] Using encoder: %s", encoders[i]);
            
            // 獲取編碼器的像素格式
            AVPixelFormat pix_fmt = ctx_->pix_fmt;
            blog(LOG_INFO, "[FFmpeg] Encoder pixel format: %d", (int)pix_fmt);
            
            // 分配 frame 和 packet（使用編碼器的像素格式）
            frame_ = av_frame_alloc();
            frame_->format = pix_fmt;
            frame_->width = width;
            frame_->height = height;
            if (av_frame_get_buffer(frame_, 0) < 0) {
                blog(LOG_ERROR, "[FFmpeg] Failed to allocate frame buffer");
                reset();
                continue;  // 嘗試下一個編碼器
            }
            
            pkt_ = av_packet_alloc();
            
            // 創建 BGRA → 編碼器格式 轉換器
            sws_ = sws_getContext(
                width, height, AV_PIX_FMT_BGRA,
                width, height, pix_fmt,
                SWS_BILINEAR, nullptr, nullptr, nullptr
            );
            
            if (!sws_) {
                blog(LOG_ERROR, "[FFmpeg] Failed to create swscale context");
                reset();
                continue;  // 嘗試下一個編碼器
            }
            
            return true;
        }
    }
    
    blog(LOG_ERROR, "[FFmpeg] No H.264 encoder available!");
    return false;
}

bool FFmpegEncoder::encode(const uint8_t* bgra_data, uint32_t width, uint32_t height,
                            uint32_t linesize, std::vector<uint8_t>& out_data) {
    if (!ctx_ || !frame_ || !pkt_ || !sws_) {
        return false;
    }
    
    // 檢查尺寸變化
    if (width != width_ || height != height_) {
        init(width, height);
        if (!ctx_) return false;
    }
    
    // BGRA → YUV420P
    const uint8_t* src[] = { bgra_data };
    int src_linesize[] = { (int)linesize };
    
    sws_scale(sws_, src, src_linesize, 0, height,
              frame_->data, frame_->linesize);
    
    frame_->pts = pts_++;
    
    // 編碼
    int ret = avcodec_send_frame(ctx_, frame_);
    if (ret < 0) {
        return false;
    }
    
    ret = avcodec_receive_packet(ctx_, pkt_);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return false;
    }
    if (ret < 0) {
        return false;
    }
    
    // 輸出
    out_data.assign(pkt_->data, pkt_->data + pkt_->size);
    
    av_packet_unref(pkt_);
    return true;
}

const char* FFmpegEncoder::getName() const {
    return encoder_name_.empty() ? "Unknown" : encoder_name_.c_str();
}

// ========== 解碼器實現 ==========

FFmpegDecoder::FFmpegDecoder() = default;

FFmpegDecoder::~FFmpegDecoder() {
    reset();
}

void FFmpegDecoder::reset() {
    if (sws_) {
        sws_freeContext(sws_);
        sws_ = nullptr;
    }
    if (pkt_) {
        av_packet_free(&pkt_);
    }
    if (frame_) {
        av_frame_free(&frame_);
    }
    if (ctx_) {
        avcodec_free_context(&ctx_);
    }
    last_width_ = last_height_ = 0;
}

bool FFmpegDecoder::init() {
    reset();
    
    // 使用軟體解碼器 (通常足夠快)
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        blog(LOG_ERROR, "[FFmpeg] H.264 decoder not found");
        return false;
    }
    
    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) {
        return false;
    }
    
    
    // 低延遲設定
    ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
    
    if (avcodec_open2(ctx_, codec, nullptr) < 0) {
        avcodec_free_context(&ctx_);
        return false;
    }
    
    frame_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    
    decoder_name_ = codec->name;
    blog(LOG_INFO, "[FFmpeg] Using decoder: %s", codec->name);
    
    return true;
}

bool FFmpegDecoder::decode(const uint8_t* h264_data, size_t size,
                            uint32_t expected_width, uint32_t expected_height,
                            std::vector<uint8_t>& out_bgra) {
    if (!ctx_ || !frame_ || !pkt_) {
        // 自動初始化
        if (!init()) {
            return false;
        }
    }
    
    pkt_->data = const_cast<uint8_t*>(h264_data);
    pkt_->size = (int)size;
    
    int ret = avcodec_send_packet(ctx_, pkt_);
    if (ret < 0) {
        return false;
    }
    
    ret = avcodec_receive_frame(ctx_, frame_);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return false;
    }
    if (ret < 0) {
        return false;
    }
    
    uint32_t w = frame_->width;
    uint32_t h = frame_->height;
    
    // 重新創建 SWS (尺寸變化時)
    if (w != last_width_ || h != last_height_) {
        if (sws_) sws_freeContext(sws_);
        sws_ = sws_getContext(
            w, h, (AVPixelFormat)frame_->format,
            w, h, AV_PIX_FMT_BGRA,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        last_width_ = w;
        last_height_ = h;
    }
    
    // YUV → BGRA
    out_bgra.resize(w * h * 4);
    uint8_t* dst[] = { out_bgra.data() };
    int dst_linesize[] = { (int)(w * 4) };
    
    sws_scale(sws_, frame_->data, frame_->linesize, 0, h,
              dst, dst_linesize);
    
    return true;
}

const char* FFmpegDecoder::getName() const {
    return decoder_name_.empty() ? "Unknown" : decoder_name_.c_str();
}

