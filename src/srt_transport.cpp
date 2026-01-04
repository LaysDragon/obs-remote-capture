/*
 * srt_transport.cpp
 * SRT 串流傳輸層實現
 *
 * 使用 FFmpeg 的 SRT protocol (通過 libavformat)
 * - Server: MPEG-TS muxer over SRT (listen mode)
 * - Client: MPEG-TS demuxer over SRT (caller mode)
 */

#include "srt_transport.h"

#include <obs-module.h>

#include <mutex>
#include <queue>
#include <condition_variable>
#include <chrono>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libavutil/channel_layout.h>
}

// ========== SRT Server Implementation ==========

class SrtServer::Impl {
public:
    Impl() = default;
    ~Impl() { stop(); }
    
    bool start(int port, int latency_ms, uint32_t width, uint32_t height, int fps) {
        if (running_.load()) return false;
        
        port_ = port;
        latency_ms_ = latency_ms;
        width_ = width;
        height_ = height;
        fps_ = fps;
        
        // 構建 SRT URL (listen mode)
        char url[256];
        snprintf(url, sizeof(url), 
                 "srt://0.0.0.0:%d?mode=listener&latency=%d&pkt_size=1316",
                 port, latency_ms * 1000);  // SRT latency 以微秒為單位
        
        srt_url_ = url;
        
        blog(LOG_INFO, "[SRT Server] Starting on port %d, latency=%dms", port, latency_ms);
        
        // 啟動發送線程
        running_.store(true);
        send_thread_ = std::thread(&Impl::sendLoop, this);
        
        info_.port = port;
        info_.latency_ms = latency_ms;
        info_.ready = true;
        
        return true;
    }
    
    void stop() {
        if (!running_.load()) return;
        
        running_.store(false);
        queue_cv_.notify_all();
        
        if (send_thread_.joinable()) {
            send_thread_.join();
        }
        
        closeOutput();
        
        info_.ready = false;
        blog(LOG_INFO, "[SRT Server] Stopped");
    }
    
    bool isRunning() const { return running_.load(); }
    bool hasClient() const { return has_client_.load(); }
    SrtConnectionInfo getInfo() const { return info_; }
    
    bool sendVideoFrame(const uint8_t* data, size_t size, int64_t pts_us, bool is_keyframe) {
        if (!running_.load()) return false;
        
        // 複製數據到隊列
        FrameData frame;
        frame.type = FRAME_VIDEO;
        frame.data.assign(data, data + size);
        frame.pts_us = pts_us;
        frame.is_keyframe = is_keyframe;
        
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (video_queue_.size() < 30) {  // 限制隊列大小
                video_queue_.push(std::move(frame));
            }
        }
        queue_cv_.notify_one();
        
        return true;
    }
    
    bool sendAudioFrame(const float* data, size_t samples, 
                        uint32_t sample_rate, uint32_t channels, int64_t pts_us) {
        if (!running_.load()) return false;
        
        // 直接複製 float planar 數據，前面加上 header
        // Header: [sample_rate: 4B][channels: 4B][samples: 4B] = 12 bytes
        FrameData frame;
        frame.type = FRAME_AUDIO;
        frame.samples = samples;
        frame.sample_rate = sample_rate;
        frame.channels = channels;
        frame.pts_us = pts_us;
        
        // 計算數據大小: header + float data
        size_t header_size = 3 * sizeof(uint32_t);  // 12 bytes
        size_t float_data_size = samples * channels * sizeof(float);
        frame.audio_raw.resize(header_size + float_data_size);
        
        // 寫入 header
        uint32_t* header = (uint32_t*)frame.audio_raw.data();
        header[0] = sample_rate;
        header[1] = channels;
        header[2] = (uint32_t)samples;
        
        // 寫入 float 數據
        std::memcpy(frame.audio_raw.data() + header_size, data, float_data_size);
        
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (audio_queue_.size() < 100) {
                audio_queue_.push(std::move(frame));
            }
        }
        queue_cv_.notify_one();
        
        return true;
    }
    
private:
    enum FrameType { FRAME_VIDEO, FRAME_AUDIO };
    
    struct FrameData {
        FrameType type;
        std::vector<uint8_t> data;  // Video: H.264 NAL units
        std::vector<uint8_t> audio_raw;  // Audio: header + float planar data
        size_t samples = 0;
        uint32_t sample_rate = 0;
        uint32_t channels = 0;
        int64_t pts_us = 0;
        bool is_keyframe = false;
    };
    
    void sendLoop() {
        blog(LOG_INFO, "[SRT Server] Send thread started");
        
        while (running_.load()) {
            // 等待連接 (lazy init output)
            if (!fmt_ctx_) {
                if (!initOutput()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                has_client_.store(true);
                blog(LOG_INFO, "[SRT Server] Client connected");
            }
            
            // 從隊列取出幀並發送
            FrameData video_frame, audio_frame;
            bool has_video = false, has_audio = false;
            
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_cv_.wait_for(lock, std::chrono::milliseconds(10), [this]() {
                    return !video_queue_.empty() || !audio_queue_.empty() || !running_.load();
                });
                
                if (!video_queue_.empty()) {
                    video_frame = std::move(video_queue_.front());
                    video_queue_.pop();
                    has_video = true;
                }
                if (!audio_queue_.empty()) {
                    audio_frame = std::move(audio_queue_.front());
                    audio_queue_.pop();
                    has_audio = true;
                }
            }
            
            // 發送視訊
            if (has_video && video_stream_) {
                if (!writeVideoPacket(video_frame)) {
                    blog(LOG_WARNING, "[SRT Server] Failed to write video, reconnecting...");
                    closeOutput();
                    has_client_.store(false);
                }
            }
            
            // 發送音訊
            if (has_audio && audio_stream_) {
                if (!writeAudioPacket(audio_frame)) {
                    blog(LOG_WARNING, "[SRT Server] Failed to write audio, reconnecting...");
                    closeOutput();
                    has_client_.store(false);
                }
            }
        }
        
        closeOutput();
        blog(LOG_INFO, "[SRT Server] Send thread exiting");
    }
    
    bool initOutput() {
        // 分配 MPEG-TS 輸出格式
        int ret = avformat_alloc_output_context2(&fmt_ctx_, nullptr, "mpegts", srt_url_.c_str());
        if (ret < 0 || !fmt_ctx_) {
            blog(LOG_ERROR, "[SRT Server] Failed to allocate output context");
            return false;
        }
        
        // 添加視訊流 (H.264)
        const AVCodec* video_codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!video_codec) {
            blog(LOG_ERROR, "[SRT Server] H264 codec not found");
            closeOutput();
            return false;
        }
        
        video_stream_ = avformat_new_stream(fmt_ctx_, video_codec);
        if (!video_stream_) {
            blog(LOG_ERROR, "[SRT Server] Failed to create video stream");
            closeOutput();
            return false;
        }
        
        video_stream_->id = 0;
        video_stream_->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        video_stream_->codecpar->codec_id = AV_CODEC_ID_H264;
        video_stream_->codecpar->width = width_;
        video_stream_->codecpar->height = height_;
        video_stream_->codecpar->format = AV_PIX_FMT_YUV420P;
        video_stream_->time_base = AVRational{1, 1000000};  // 微秒
        
        // 添加音訊流 (PCM S16LE)
        const AVCodec* audio_codec = avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE);
        if (audio_codec) {
            audio_stream_ = avformat_new_stream(fmt_ctx_, audio_codec);
            if (audio_stream_) {
                audio_stream_->id = 1;
                audio_stream_->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
                audio_stream_->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
                audio_stream_->codecpar->sample_rate = 48000;
                // 使用新的 channel layout API
                av_channel_layout_default(&audio_stream_->codecpar->ch_layout, 2);
                audio_stream_->codecpar->format = AV_SAMPLE_FMT_S16;
                audio_stream_->time_base = AVRational{1, 48000};
                blog(LOG_INFO, "[SRT Server] Audio stream created with PCM S16LE");
            }
        } else {
            blog(LOG_WARNING, "[SRT Server] PCM S16LE codec not found, no audio stream");
            audio_stream_ = nullptr;
        }
        
        // 設置 SRT 選項
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "mode", "listener", 0);
        av_dict_set_int(&opts, "latency", latency_ms_ * 1000, 0);
        
        // 打開輸出 (這會阻塞直到有客戶端連接)
        blog(LOG_INFO, "[SRT Server] Waiting for client connection on %s...", srt_url_.c_str());
        ret = avio_open2(&fmt_ctx_->pb, srt_url_.c_str(), AVIO_FLAG_WRITE, nullptr, &opts);
        av_dict_free(&opts);
        
        if (ret < 0) {
            char err[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, err, sizeof(err));
            blog(LOG_ERROR, "[SRT Server] Failed to open SRT output: %s", err);
            closeOutput();
            return false;
        }
        
        // 寫入頭部
        ret = avformat_write_header(fmt_ctx_, nullptr);
        if (ret < 0) {
            char err[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, err, sizeof(err));
            blog(LOG_ERROR, "[SRT Server] Failed to write header: %s", err);
            closeOutput();
            return false;
        }
        
        blog(LOG_INFO, "[SRT Server] Output initialized, streams: video=%d audio=%d",
             video_stream_ ? video_stream_->index : -1,
             audio_stream_ ? audio_stream_->index : -1);
        
        return true;
    }
    
    void closeOutput() {
        if (fmt_ctx_) {
            if (fmt_ctx_->pb) {
                av_write_trailer(fmt_ctx_);
                avio_closep(&fmt_ctx_->pb);
            }
            avformat_free_context(fmt_ctx_);
            fmt_ctx_ = nullptr;
        }
        video_stream_ = nullptr;
        audio_stream_ = nullptr;
    }
    
    bool writeVideoPacket(const FrameData& frame) {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) return false;
        
        pkt->data = (uint8_t*)frame.data.data();
        pkt->size = (int)frame.data.size();
        pkt->pts = av_rescale_q(frame.pts_us, AVRational{1, 1000000}, video_stream_->time_base);
        pkt->dts = pkt->pts;
        pkt->stream_index = video_stream_->index;
        if (frame.is_keyframe) {
            pkt->flags |= AV_PKT_FLAG_KEY;
        }
        
        int ret = av_interleaved_write_frame(fmt_ctx_, pkt);
        av_packet_free(&pkt);
        
        return ret >= 0;
    }
    
    bool writeAudioPacket(const FrameData& frame) {
        if (!audio_stream_) return true;  // 沒有音訊流，跳過
        
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) return false;
        
        pkt->data = (uint8_t*)frame.audio_raw.data();
        pkt->size = (int)frame.audio_raw.size();
        pkt->pts = av_rescale_q(frame.pts_us, AVRational{1, 1000000}, audio_stream_->time_base);
        pkt->dts = pkt->pts;
        pkt->stream_index = audio_stream_->index;
        
        int ret = av_interleaved_write_frame(fmt_ctx_, pkt);
        av_packet_free(&pkt);
        
        return ret >= 0;
    }
    
    // 狀態
    std::atomic<bool> running_{false};
    std::atomic<bool> has_client_{false};
    SrtConnectionInfo info_;
    
    // 配置
    int port_ = 0;
    int latency_ms_ = 200;
    uint32_t width_ = 1920;
    uint32_t height_ = 1080;
    int fps_ = 30;
    std::string srt_url_;
    
    // FFmpeg
    AVFormatContext* fmt_ctx_ = nullptr;
    AVStream* video_stream_ = nullptr;
    AVStream* audio_stream_ = nullptr;
    
    // 發送線程和隊列
    std::thread send_thread_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<FrameData> video_queue_;
    std::queue<FrameData> audio_queue_;
};

SrtServer::SrtServer() : impl_(std::make_unique<Impl>()) {}
SrtServer::~SrtServer() = default;

bool SrtServer::start(int port, int latency_ms, uint32_t width, uint32_t height, int fps) {
    return impl_->start(port, latency_ms, width, height, fps);
}

void SrtServer::stop() { impl_->stop(); }
bool SrtServer::isRunning() const { return impl_->isRunning(); }
bool SrtServer::hasClient() const { return impl_->hasClient(); }
SrtConnectionInfo SrtServer::getInfo() const { return impl_->getInfo(); }

bool SrtServer::sendVideoFrame(const uint8_t* data, size_t size, int64_t pts_us, bool is_keyframe) {
    return impl_->sendVideoFrame(data, size, pts_us, is_keyframe);
}

bool SrtServer::sendAudioFrame(const float* data, size_t samples,
                                uint32_t sample_rate, uint32_t channels, int64_t pts_us) {
    return impl_->sendAudioFrame(data, samples, sample_rate, channels, pts_us);
}


// ========== SRT Client Implementation ==========

class SrtClient::Impl {
public:
    Impl() = default;
    ~Impl() { disconnect(); }
    
    bool connect(const std::string& host, int port, int latency_ms) {
        if (connected_.load()) return false;
        
        host_ = host;
        port_ = port;
        latency_ms_ = latency_ms;
        
        // 構建 SRT URL (caller mode)
        char url[256];
        snprintf(url, sizeof(url), 
                 "srt://%s:%d?mode=caller&latency=%d&connect_timeout=5000",
                 host.c_str(), port, latency_ms * 1000);
        
        srt_url_ = url;
        
        blog(LOG_INFO, "[SRT Client] Connecting to %s:%d, latency=%dms", 
             host.c_str(), port, latency_ms);
        
        // 打開輸入 - 設置分析時間參數以加快格式探測
        AVDictionary* opts = nullptr;
        // 減少分析時間（默認 5 秒可能太長）
        av_dict_set_int(&opts, "analyzeduration", 1000000, 0);  // 1 秒
        av_dict_set_int(&opts, "probesize", 32768, 0);  // 32KB
        
        int ret = avformat_open_input(&fmt_ctx_, srt_url_.c_str(), nullptr, &opts);
        av_dict_free(&opts);
        
        if (ret < 0) {
            char err[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, err, sizeof(err));
            blog(LOG_ERROR, "[SRT Client] Failed to connect: %s (error code: %d)", err, ret);
            return false;
        }
        
        // 獲取流信息 - 設置較短的分析時間
        fmt_ctx_->max_analyze_duration = 1000000;  // 1 秒
        ret = avformat_find_stream_info(fmt_ctx_, nullptr);
        if (ret < 0) {
            char err[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, err, sizeof(err));
            blog(LOG_ERROR, "[SRT Client] Failed to find stream info: %s", err);
            disconnect();
            return false;
        }
        
        // 找到視訊和音訊流
        // 注意: MPEG-TS 會把 PCM S16LE 標記為 bin_data (type=2)，我們需要特別處理
        blog(LOG_INFO, "[SRT Client] Found %d streams", fmt_ctx_->nb_streams);
        for (unsigned i = 0; i < fmt_ctx_->nb_streams; i++) {
            AVCodecParameters* par = fmt_ctx_->streams[i]->codecpar;
            const char* codec_name = avcodec_get_name(par->codec_id);
            blog(LOG_INFO, "[SRT Client] Stream %d: type=%d codec=%s (%d)", 
                 i, par->codec_type, codec_name, par->codec_id);
            
            if (par->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_idx_ < 0) {
                video_stream_idx_ = i;
                blog(LOG_INFO, "[SRT Client] Using video stream %d: %dx%d", 
                     i, par->width, par->height);
            } else if (par->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream_idx_ < 0) {
                audio_stream_idx_ = i;
                blog(LOG_INFO, "[SRT Client] Using audio stream %d: %dHz %dch", 
                     i, par->sample_rate, par->ch_layout.nb_channels);
            } else if (par->codec_type == AVMEDIA_TYPE_DATA && audio_stream_idx_ < 0) {
                // MPEG-TS 把我們的自定義音訊數據標記為 DATA/bin_data
                audio_stream_idx_ = i;
                blog(LOG_INFO, "[SRT Client] Using DATA stream %d as raw audio", i);
            }
        }
        
        if (video_stream_idx_ < 0) {
            blog(LOG_WARNING, "[SRT Client] No video stream found");
        }
        if (audio_stream_idx_ < 0) {
            blog(LOG_WARNING, "[SRT Client] No audio stream found");
        }
        
        connected_.store(true);
        blog(LOG_INFO, "[SRT Client] Connected");
        
        return true;
    }
    
    void disconnect() {
        stopReceive();
        
        if (fmt_ctx_) {
            avformat_close_input(&fmt_ctx_);
            fmt_ctx_ = nullptr;
        }
        
        video_stream_idx_ = -1;
        audio_stream_idx_ = -1;
        connected_.store(false);
        
        blog(LOG_INFO, "[SRT Client] Disconnected");
    }
    
    bool isConnected() const { return connected_.load(); }
    bool isReceiving() const { return receiving_.load(); }
    
    bool startReceive(VideoCallback on_video, AudioCallback on_audio) {
        if (!connected_.load() || receiving_.load()) return false;
        
        on_video_ = on_video;
        on_audio_ = on_audio;
        
        receiving_.store(true);
        receive_thread_ = std::thread(&Impl::receiveLoop, this);
        
        blog(LOG_INFO, "[SRT Client] Started receiving");
        return true;
    }
    
    void stopReceive() {
        if (!receiving_.load()) return;
        
        receiving_.store(false);
        
        if (receive_thread_.joinable()) {
            receive_thread_.join();
        }
        
        blog(LOG_INFO, "[SRT Client] Stopped receiving");
    }
    
private:
    void receiveLoop() {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) {
            blog(LOG_ERROR, "[SRT Client] Failed to allocate packet");
            return;
        }
        
        while (receiving_.load() && connected_.load()) {
            int ret = av_read_frame(fmt_ctx_, pkt);
            if (ret < 0) {
                if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                char err[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, err, sizeof(err));
                blog(LOG_WARNING, "[SRT Client] Read error: %s", err);
                break;
            }
            
            if (pkt->stream_index == video_stream_idx_ && on_video_) {
                AVStream* st = fmt_ctx_->streams[video_stream_idx_];
                int64_t pts_us = av_rescale_q(pkt->pts, st->time_base, AVRational{1, 1000000});
                bool is_keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
                
                on_video_(pkt->data, pkt->size, pts_us, is_keyframe);
            } else if (pkt->stream_index == audio_stream_idx_ && on_audio_) {
                AVStream* st = fmt_ctx_->streams[audio_stream_idx_];
                int64_t pts_us = av_rescale_q(pkt->pts, st->time_base, AVRational{1, 1000000});
                
                // 解析 header: [sample_rate: 4B][channels: 4B][samples: 4B]
                const size_t header_size = 3 * sizeof(uint32_t);  // 12 bytes
                if (pkt->size < (int)header_size) {
                    blog(LOG_WARNING, "[SRT Client] Audio packet too small: %d bytes", pkt->size);
                    av_packet_unref(pkt);
                    continue;
                }
                
                const uint32_t* header = (const uint32_t*)pkt->data;
                uint32_t sample_rate = header[0];
                uint32_t channels = header[1];
                size_t samples = header[2];
                
                // 驗證數據大小
                size_t expected_size = header_size + samples * channels * sizeof(float);
                if (pkt->size != (int)expected_size) {
                    blog(LOG_WARNING, "[SRT Client] Audio size mismatch: got %d, expected %zu", 
                         pkt->size, expected_size);
                    av_packet_unref(pkt);
                    continue;
                }
                
                // 直接使用 float planar 數據
                const float* float_data = (const float*)(pkt->data + header_size);
                
                on_audio_(float_data, samples, sample_rate, channels, pts_us);
            }
            
            av_packet_unref(pkt);
        }
        
        av_packet_free(&pkt);
        blog(LOG_INFO, "[SRT Client] Receive loop exiting");
    }
    
    // 狀態
    std::atomic<bool> connected_{false};
    std::atomic<bool> receiving_{false};
    
    // 配置
    std::string host_;
    int port_ = 0;
    int latency_ms_ = 200;
    std::string srt_url_;
    
    // FFmpeg
    AVFormatContext* fmt_ctx_ = nullptr;
    int video_stream_idx_ = -1;
    int audio_stream_idx_ = -1;
    
    // 接收線程
    std::thread receive_thread_;
    VideoCallback on_video_;
    AudioCallback on_audio_;
};

SrtClient::SrtClient() : impl_(std::make_unique<Impl>()) {}
SrtClient::~SrtClient() = default;

bool SrtClient::connect(const std::string& host, int port, int latency_ms) {
    return impl_->connect(host, port, latency_ms);
}

void SrtClient::disconnect() { impl_->disconnect(); }
bool SrtClient::isConnected() const { return impl_->isConnected(); }
bool SrtClient::isReceiving() const { return impl_->isReceiving(); }

bool SrtClient::startReceive(VideoCallback on_video, AudioCallback on_audio) {
    return impl_->startReceive(on_video, on_audio);
}

void SrtClient::stopReceive() { impl_->stopReceive(); }
