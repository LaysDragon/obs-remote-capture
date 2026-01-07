/*
 * remote-client-source.cpp
 * 接收端來源 - Session-based API
 *
 * 功能:
 * 1. 連接 gRPC 服務器並創建 Session
 * 2. 獲取可用 source 列表，選擇 source type
 * 3. 動態接收屬性更新，完全被動
 * 4. 串流綁定 session_id
 */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/threading.h>
#include <util/platform.h>

#include "grpc_client.h"

// 禁用 protobuf 生成代碼的警告
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4267)
#endif

#include "remote_capture.pb.h"

#ifdef _MSC_VER
#pragma warning(pop)
#endif

using namespace obsremote;

#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <vector>
#include <string>
#include <cstring>
#include <sstream>
#include <map>
#include <chrono>
#include <cmath>  // for sqrt()

// H.264 解碼器
#include "codec_ffmpeg.h"
#include "plugin-utils.h"
#include "srt_transport.h"

#define DEFAULT_SERVER_PORT 44555

// ========== 來源數據結構 ==========
struct remote_source_data {
    obs_source_t* source;

    // 連接設定
    std::string server_ip;
    int server_port;

    // gRPC 客戶端
    std::unique_ptr<GrpcClient> grpc_client;
    std::atomic<bool> connected;
    std::atomic<bool> streaming;
    
    // Session
    std::string session_id;
    std::string current_source_type;
    
    // 錯誤訊息 (用於 UI 顯示)
    std::mutex error_mutex;
    std::string last_error;
    
    // 可用 source 列表 (從 server 獲取)
    std::vector<GrpcClient::SourceInfo> available_sources;
    
    // 當前屬性緩存 (完全動態，從 server 獲取)
    std::mutex props_mutex;
    std::vector<GrpcClient::Property> cached_props;

    // 視頻幀尺寸 (用於 get_width/get_height)
    std::atomic<uint32_t> video_width{0};
    std::atomic<uint32_t> video_height{0};

    // 音頻 (從服務端獲取，不是本地設定)
    bool has_audio{false};

    // H.264 解碼器
    std::unique_ptr<FFmpegDecoder> h264_decoder;
    
    // 連線設定 debounce (3秒)
    std::string pending_ip;
    int pending_port{0};
    std::chrono::steady_clock::time_point pending_connect_time;
    bool pending_connect{false};
    
    // SRT 傳輸
    std::unique_ptr<SrtClient> srt_client;
    bool use_srt{true};  // 是否使用 SRT (vs gRPC fallback)
    int srt_port{0};
    int srt_latency_ms{200};
    
    // 時鐘同步資訊 (用於 E2E 延遲計算)
    std::atomic<int64_t> clock_offset_ns{0};       // client_time - server_time
    std::atomic<int64_t> stream_start_time_ns{0};  // Server 端的 stream start time
    std::atomic<bool> clock_sync_valid{false};

    remote_source_data() :
        source(nullptr),
        server_port(DEFAULT_SERVER_PORT),
        connected(false),
        streaming(false)
    {}
    
    void setError(const std::string& msg) {
        std::lock_guard<std::mutex> lock(error_mutex);
        last_error = msg;
        blog(LOG_WARNING, "[Remote Source] Error: %s", msg.c_str());
    }
    
    void clearError() {
        std::lock_guard<std::mutex> lock(error_mutex);
        last_error.clear();
    }
    
    std::string getError() {
        std::lock_guard<std::mutex> lock(error_mutex);
        return last_error;
    }
};
// ========== 前向聲明 ==========
static void remote_source_update(void* data_ptr, obs_data_t* settings);
static void schedule_audio_active_check(remote_source_data* data);

// ========== 連接伺服器並創建 Session ==========
static bool connect_and_create_session(remote_source_data* data) {
    if (!data) return false;
    if (data->connected.load()) return true;
    if (data->server_ip.empty()) {
        data->setError("Server IP not configured");
        return false;
    }

    data->clearError();
    std::string address = data->server_ip + ":" + std::to_string(data->server_port);
    
    try {
        data->grpc_client = std::make_unique<GrpcClient>(address);
    } catch (const std::exception& e) {
        data->setError(std::string("Failed to create client: ") + e.what());
        return false;
    }
    
    if (!data->grpc_client) {
        data->setError("Failed to create gRPC client");
        return false;
    }
    
    if (!data->grpc_client->waitForConnected(1000)) {
        data->setError("Connection timeout: " + address);
        data->grpc_client.reset();
        return false;
    }
    
    blog(LOG_INFO, "[Remote Source] Connected to %s", address.c_str());
    
    // 獲取可用 source 列表
    if (!data->grpc_client->getAvailableSources(data->available_sources)) {
        data->setError("Failed to get available sources");
        // 不中斷，繼續嘗試
    } else {
        blog(LOG_INFO, "[Remote Source] Got %zu available sources", data->available_sources.size());
    }
    
    // 創建 session
    if (!data->grpc_client->createSession(data->session_id)) {
        data->setError("Failed to create session");
        data->grpc_client.reset();
        return false;
    }
    
    blog(LOG_INFO, "[Remote Source] Created session: %s", data->session_id.c_str());
    data->connected.store(true);
    data->clearError();
    return true;
}

static void disconnect_and_release_session(remote_source_data* data) {
    data->streaming.store(false);
    
    if (data->grpc_client) {
        data->grpc_client->stopStream();
        
        if (!data->session_id.empty()) {
            data->grpc_client->releaseSession(data->session_id);
            data->session_id.clear();
        }
        
        data->grpc_client.reset();
    }
    
    data->connected.store(false);
    data->current_source_type.clear();
}

// ========== gRPC 回調函數 ==========
// 客戶端性能測量變數 (static 保持跨幀狀態)
static struct {
    std::chrono::steady_clock::time_point last_recv_time{};
    
    int64_t max_recv_interval_ms = 0;
    int64_t max_decode_ms = 0;
    int64_t max_recv_to_ready_ms = 0;  // 從接收到解碼完成的本地延遲
    int64_t max_e2e_ms = 0;            // E2E: 從 Server 捕獲到 Client 解碼完成
    int64_t total_recv_interval_ms = 0;
    int64_t total_decode_ms = 0;
    int64_t total_recv_to_ready_ms = 0;
    int64_t total_e2e_ms = 0;
    int64_t total_e2e_sq_ms = 0;       // E2E 平方和 (用於計算變異數)
    uint32_t perf_frame_count = 0;
    
    // 自適應 jitter buffer 參數 (持久化，不每 30 幀重置)
    double running_avg_e2e_ms = 100.0;   // EMA 平均 E2E
    double running_var_e2e_ms = 0.0;     // EMA 變異數
    int64_t jitter_buffer_ns = 150 * 1000000;  // 初始 150ms
    static constexpr double EMA_ALPHA = 0.1;   // EMA 平滑係數
    static constexpr int64_t MARGIN_MS = 50;   // 固定安全邊際
} client_perf;

static void grpc_video_callback(uint32_t width, uint32_t height,
                                 int codec,
                                 const uint8_t* frame_data, size_t frame_size,
                                 uint32_t linesize,
                                 uint64_t timestamp_ns, void* user_data) {
    UNUSED_PARAMETER(linesize);
    remote_source_data* data = (remote_source_data*)user_data;
    if (!data) return;
    
    auto recv_time = std::chrono::steady_clock::now();
    
    static uint32_t callback_count = 0;
    callback_count++;
    
    if (codec != VideoCodec::CODEC_H264) {
        if (callback_count % 100 == 1) {
            blog(LOG_WARNING, "[Remote Source] Unsupported codec: %d", codec);
        }
        return;
    }
    
    // 計算接收間隔 (與上一幀的時間差)
    int64_t recv_interval_ms = 0;
    if (client_perf.last_recv_time.time_since_epoch().count() > 0) {
        recv_interval_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            recv_time - client_perf.last_recv_time).count();
    }
    client_perf.last_recv_time = recv_time;
    
    // 確保解碼器存在
    if (!data->h264_decoder) {
        data->h264_decoder = std::make_unique<FFmpegDecoder>();
        if (!data->h264_decoder->init()) {
            blog(LOG_ERROR, "[Remote Source] Failed to create H.264 decoder");
            data->h264_decoder.reset();
            return;
        }
        blog(LOG_INFO, "[Remote Source] Created H.264 decoder: %s", 
             data->h264_decoder->getName());
    }
    
    // 解碼計時
    auto decode_start = std::chrono::steady_clock::now();
    
    std::vector<uint8_t> decoded;
    uint32_t decoded_width = 0, decoded_height = 0;
    if (data->h264_decoder->decode(frame_data, frame_size, width, height, 
                                     decoded, decoded_width, decoded_height)) {
        auto decode_end = std::chrono::steady_clock::now();
        int64_t decode_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            decode_end - decode_start).count();
        
        // 從接收到解碼完成的本地延遲 (不跨機器)
        int64_t recv_to_ready_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            decode_end - recv_time).count();
        
        // E2E 延遲計算
        // 現在 timestamp_ns 已經由 grpc_client 轉換為 client 時間參考 (os_gettime_ns)
        // 因此: e2e = 解碼完成時間 - 捕獲時間 (都是 client 端的 os_gettime_ns)
        int64_t e2e_ms = 0;
        if (timestamp_ns > 0) {
            int64_t decode_end_ns = os_gettime_ns();
            e2e_ms = (decode_end_ns - (int64_t)timestamp_ns) / 1000000;  // ns to ms
        }
        
        // 更新統計
        client_perf.perf_frame_count++;
        client_perf.total_recv_interval_ms += recv_interval_ms;
        client_perf.total_decode_ms += decode_ms;
        client_perf.total_recv_to_ready_ms += recv_to_ready_ms;
        client_perf.total_e2e_ms += e2e_ms;
        client_perf.total_e2e_sq_ms += e2e_ms * e2e_ms;  // 平方和
        if (recv_interval_ms > client_perf.max_recv_interval_ms) 
            client_perf.max_recv_interval_ms = recv_interval_ms;
        if (decode_ms > client_perf.max_decode_ms) 
            client_perf.max_decode_ms = decode_ms;
        if (recv_to_ready_ms > client_perf.max_recv_to_ready_ms) 
            client_perf.max_recv_to_ready_ms = recv_to_ready_ms;
        if (e2e_ms > client_perf.max_e2e_ms) 
            client_perf.max_e2e_ms = e2e_ms;
        
        // 更新 EMA 平均和變異數 (每幀更新)
        double delta = (double)e2e_ms - client_perf.running_avg_e2e_ms;
        client_perf.running_avg_e2e_ms += client_perf.EMA_ALPHA * delta;
        client_perf.running_var_e2e_ms = (1.0 - client_perf.EMA_ALPHA) * 
            (client_perf.running_var_e2e_ms + client_perf.EMA_ALPHA * delta * delta);
        
        // 計算自適應 jitter buffer: avg + 2*stddev + margin
        double stddev_ms = sqrt(client_perf.running_var_e2e_ms);
        double buffer_ms = client_perf.running_avg_e2e_ms + 2.0 * stddev_ms + client_perf.MARGIN_MS;
        client_perf.jitter_buffer_ns = (int64_t)(buffer_ms * 1000000.0);
        
        // 每 30 幀輸出統計
        if (client_perf.perf_frame_count % 30 == 0) {
            int64_t avg_recv = client_perf.total_recv_interval_ms / 30;
            int64_t avg_decode = client_perf.total_decode_ms / 30;
            int64_t avg_ready = client_perf.total_recv_to_ready_ms / 30;
            int64_t avg_e2e = client_perf.total_e2e_ms / 30;
            blog(LOG_INFO, "[Client Perf] recv_interval avg=%lldms max=%lldms, decode avg=%lldms max=%lldms, recv_to_ready avg=%lldms max=%lldms, e2e avg=%lldms max=%lldms",
                 avg_recv, client_perf.max_recv_interval_ms,
                 avg_decode, client_perf.max_decode_ms,
                 avg_ready, client_perf.max_recv_to_ready_ms,
                 avg_e2e, client_perf.max_e2e_ms);
            blog(LOG_INFO, "[Jitter Buffer] ema_avg=%.1fms, stddev=%.1fms, buffer=%.1fms",
                 client_perf.running_avg_e2e_ms, stddev_ms, buffer_ms);
            // 重置 30 幀統計 (但保留 EMA 和 jitter_buffer_ns)
            client_perf.total_recv_interval_ms = 0;
            client_perf.total_decode_ms = 0;
            client_perf.total_recv_to_ready_ms = 0;
            client_perf.total_e2e_ms = 0;
            client_perf.total_e2e_sq_ms = 0;
            client_perf.max_recv_interval_ms = 0;
            client_perf.max_decode_ms = 0;
            client_perf.max_recv_to_ready_ms = 0;
            client_perf.max_e2e_ms = 0;
        }
        
        // 更新尺寸供 get_width/get_height 使用
        data->video_width.store(decoded_width);
        data->video_height.store(decoded_height);
        
        // 使用 OBS async video API 輸出視訊幀
        // 應用自適應 jitter buffer: timestamp + buffer_delay 讓幀「在未來」
        // OBS 會等待正確時間再顯示，音視訊保持同步
        struct obs_source_frame frame = {};
        frame.width = decoded_width;
        frame.height = decoded_height;
        frame.format = VIDEO_FORMAT_BGRA;  // FFmpeg decoder 輸出 BGRA
        frame.timestamp = timestamp_ns + client_perf.jitter_buffer_ns;
        frame.data[0] = decoded.data();
        frame.linesize[0] = decoded_width * 4;  // BGRA = 4 bytes per pixel
        frame.full_range = true;
        
        obs_source_output_video(data->source, &frame);
    }
}

static void grpc_audio_callback(uint32_t sample_rate, uint32_t channels,
                                 const float* frame_data, size_t samples,
                                 uint64_t timestamp_ns, void* user_data) {
    remote_source_data* data = (remote_source_data*)user_data;
    if (!data || !data->has_audio || !data->streaming.load()) return;
    
    struct obs_source_audio audio = {};
    // frame_data 是 planar 格式: [L|L|L|...|R|R|R|...]
    // 左聲道在前半，右聲道在後半
    audio.data[0] = (uint8_t*)frame_data;  // Left channel
    if (channels >= 2) {
        audio.data[1] = (uint8_t*)(frame_data + samples);  // Right channel
    }
    audio.frames = (uint32_t)samples;
    audio.speakers = (channels == 2) ? SPEAKERS_STEREO : SPEAKERS_MONO;
    audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
    audio.samples_per_sec = sample_rate;
    // 應用相同的 jitter buffer offset，保持 A/V 同步
    audio.timestamp = timestamp_ns + client_perf.jitter_buffer_ns;
    
    obs_source_output_audio(data->source, &audio);
}

// ========== 延遲音頻狀態檢查 (類似 capture-preview) ==========
struct audio_check_data {
    remote_source_data* data;
};

static void deferred_audio_active_check(void* param) {
    os_sleep_ms(50);  // 等待 source 初始化完成
    
    audio_check_data* check = (audio_check_data*)param;
    if (!check || !check->data) {
        delete check;
        return;
    }
    
    remote_source_data* data = check->data;
    
    if (data->grpc_client && !data->session_id.empty()) {
        bool audio_active = false;
        if (data->grpc_client->isAudioActive(data->session_id, audio_active)) {
            data->has_audio = audio_active;
            obs_source_set_audio_active(data->source, audio_active);
            blog(LOG_INFO, "[Remote Source] Deferred audio check: session=%s, audio_active=%d",
                 data->session_id.c_str(), audio_active);
        }
    }
    
    delete check;
}

static void schedule_audio_active_check(remote_source_data* data) {
    if (!data || !data->source) return;
    
    audio_check_data* check = new audio_check_data();
    check->data = data;
    
    obs_queue_task(OBS_TASK_UI, deferred_audio_active_check, check, false);
}

// ========== SRT 回調函數 ==========
static void srt_video_callback(const uint8_t* h264_data, size_t size,
                                int64_t pts_ns, bool is_keyframe, void* user_data) {
    UNUSED_PARAMETER(is_keyframe);
    
    remote_source_data* data = (remote_source_data*)user_data;
    if (!data || !data->streaming.load()) return;
    
    // SRT 傳來的是相對 PTS，需要轉換為 client timestamp
    // 1. 還原 server 捕獲時間: server_capture_time = stream_start_time + relative_pts
    // 2. 轉換到 client 時間: client_capture_time = server_capture_time + clock_offset
    int64_t client_timestamp_ns = pts_ns;  // fallback: 直接使用相對 PTS
    if (data->clock_sync_valid.load() && data->stream_start_time_ns.load() > 0) {
        int64_t server_capture_time_ns = data->stream_start_time_ns.load() + pts_ns;
        client_timestamp_ns = server_capture_time_ns + data->clock_offset_ns.load();
    }
    
    // 使用 gRPC 相同的視訊回調 (codec = H264)
    grpc_video_callback(0, 0, 2,  // codec = CODEC_H264 = 2
                        h264_data, size, 0, 
                        (uint64_t)client_timestamp_ns,
                        user_data);
}

static void srt_audio_callback(const float* pcm_data, size_t samples,
                                uint32_t sample_rate, uint32_t channels,
                                int64_t pts_ns, void* user_data) {
    remote_source_data* data = (remote_source_data*)user_data;
    if (!data || !data->streaming.load()) return;

        // SRT 傳來的是相對 PTS，需要轉換為 client timestamp
    // 1. 還原 server 捕獲時間: server_capture_time = stream_start_time + relative_pts
    // 2. 轉換到 client 時間: client_capture_time = server_capture_time + clock_offset
    int64_t client_timestamp_ns = pts_ns;  // fallback: 直接使用相對 PTS
    if (data->clock_sync_valid.load() && data->stream_start_time_ns.load() > 0) {
        int64_t server_capture_time_ns = data->stream_start_time_ns.load() + pts_ns;
        client_timestamp_ns = server_capture_time_ns + data->clock_offset_ns.load();
    }
    
    // 直接調用 gRPC 音訊回調
    grpc_audio_callback(sample_rate, channels, pcm_data, samples, 
                        (uint64_t)client_timestamp_ns,  // 已是納秒
                        user_data);
}

// ========== 開始串流 ==========
static void start_streaming(remote_source_data* data) {
    if (data->streaming.load()) return;
    if (data->session_id.empty()) return;
    
    blog(LOG_INFO, "[Remote Source] Starting stream for session %s", data->session_id.c_str());
    
    data->streaming.store(true);
    
    // 先獲取 SRT 資訊（知道 port）
    int srt_port = 0;
    int srt_latency_ms = 100;
    bool want_srt = data->use_srt && data->grpc_client;
    
    if (want_srt) {
        GrpcClient::SrtInfo srt_info;
        if (data->grpc_client->getSrtInfo(data->session_id, srt_info)) {
            srt_port = srt_info.port;
            srt_latency_ms = srt_info.latency_ms;
            data->srt_port = srt_port;
            data->srt_latency_ms = srt_latency_ms;
            
            blog(LOG_INFO, "[Remote Source] Got SRT info: port=%d, latency=%dms",
                 srt_port, srt_latency_ms);
        } else {
            want_srt = false;
        }
    }
    
    remote_source_data* capture_data = data;
    
    // 執行時鐘同步 (RTT 測量，5 輪，排除異常值)
    // 這會讓 grpc_client 內部存儲 clock_offset，並在 callback 時自動轉換 timestamp
    GrpcClient::ClockSyncResult sync_result;
    if (data->grpc_client->syncClock(sync_result)) {
        data->clock_offset_ns = sync_result.clock_offset_ns;
        data->clock_sync_valid = true;
        blog(LOG_INFO, "[Remote Source] Clock sync completed: offset=%.2f ms, rtt=%.2f ms",
             sync_result.clock_offset_ns / 1000000.0,
             sync_result.rtt_ns / 1000000.0);
    } else {
        data->clock_sync_valid = false;
        blog(LOG_WARNING, "[Remote Source] Clock sync failed, e2e timing may be inaccurate");
    }
    
    // 先啟動 gRPC stream（這會觸發服務器啟動 SRT）
    // 視訊和音訊: 當 SRT 連接時忽略 gRPC 數據，否則使用 gRPC
    if (!data->grpc_client->startStream(data->session_id,
            [capture_data](uint32_t w, uint32_t h, int codec, 
                           const uint8_t* d, size_t s, uint32_t ls, uint64_t t) {
                // 視訊: 只有當 SRT 未連接時才使用 gRPC
                if (!capture_data->srt_client || !capture_data->srt_client->isReceiving()) {
                    grpc_video_callback(w, h, codec, d, s, ls, t, capture_data);
                }
            },
            [capture_data](uint32_t sr, uint32_t ch, const float* d, size_t s, uint64_t t) {
                // 音訊: 只有當 SRT 未連接時才使用 gRPC
                if (!capture_data->srt_client || !capture_data->srt_client->isReceiving()) {
                    grpc_audio_callback(sr, ch, d, s, t, capture_data);
                }
            },
            [capture_data](int64_t stream_start_time_ns) {
                // StreamInfo 回調: 存儲 stream start time 供 SRT 使用
                capture_data->stream_start_time_ns = stream_start_time_ns;
                blog(LOG_INFO, "[Remote Source] Stored stream_start_time_ns=%lld for SRT",
                     (long long)stream_start_time_ns);
            })) {
        blog(LOG_WARNING, "[Remote Source] Failed to start gRPC stream");
        data->streaming.store(false);
        return;
    }
    
    blog(LOG_INFO, "[Remote Source] gRPC stream started");
    
    // 如果要使用 SRT，在背景嘗試連接
    if (want_srt && srt_port > 0) {
        std::thread([capture_data, srt_port, srt_latency_ms]() {
            // 等待服務器 SRT 就緒（輪詢）
            for (int retry = 0; retry < 10; retry++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                
                if (!capture_data->streaming.load()) return;
                
                // 檢查 SRT 是否就緒
                GrpcClient::SrtInfo srt_info;
                if (capture_data->grpc_client->getSrtInfo(capture_data->session_id, srt_info)) {
                    if (srt_info.ready) {
                        blog(LOG_INFO, "[Remote Source] SRT server ready, attempting connection...");
                        
                        // 創建 SRT 客戶端並連接
                        capture_data->srt_client = std::make_unique<SrtClient>();
                        if (capture_data->srt_client->connect(
                                capture_data->server_ip, srt_port, srt_latency_ms)) {
                            
                            blog(LOG_INFO, "[Remote Source] SRT client connected to %s:%d",
                                 capture_data->server_ip.c_str(), srt_port);
                            
                            // 啟動 SRT 接收
                            capture_data->srt_client->startReceive(
                                [capture_data](const uint8_t* h264_data, size_t size, 
                                               int64_t pts_us, bool is_keyframe) {
                                    srt_video_callback(h264_data, size, pts_us, is_keyframe, capture_data);
                                },
                                [capture_data](const float* pcm_data, size_t samples,
                                               uint32_t sample_rate, uint32_t channels,
                                               int64_t pts_us) {
                                    srt_audio_callback(pcm_data, samples, sample_rate, channels, 
                                                        pts_us, capture_data);
                                }
                            );
                            
                            blog(LOG_INFO, "[Remote Source] SRT receive started");
                            return;
                        } else {
                            blog(LOG_WARNING, "[Remote Source] SRT connect failed, continuing with gRPC");
                            capture_data->srt_client.reset();
                            return;
                        }
                    }
                }
                
                blog(LOG_INFO, "[Remote Source] Waiting for SRT server... (%d/10)", retry + 1);
            }
            
            blog(LOG_WARNING, "[Remote Source] SRT server not ready after timeout, using gRPC only");
        }).detach();
    }
    
    blog(LOG_INFO, "[Remote Source] Stream started (gRPC%s)", 
         want_srt ? " + SRT pending" : " only");
}

static void stop_streaming(remote_source_data* data) {
    if (!data->streaming.load()) return;
    
    data->streaming.store(false);
    
    // 停止 SRT 接收 (SrtClient 內部會 join 自己的線程)
    if (data->srt_client) {
        data->srt_client->stopReceive();
        data->srt_client->disconnect();
        data->srt_client.reset();
    }
    
    // 停止 gRPC stream
    if (data->grpc_client) {
        data->grpc_client->stopStream();
    }
    
    // 重置 perf 追蹤狀態 (下次連接時重新初始化)
    client_perf.last_recv_time = std::chrono::steady_clock::time_point{};
    
    blog(LOG_INFO, "[Remote Source] Stream stopped");
}

// ========== OBS 來源回調 ==========
static const char* remote_source_get_name(void* unused) {
    UNUSED_PARAMETER(unused);
    return "Remote Source (遠端來源)";
}

static void* remote_source_create(obs_data_t* settings, obs_source_t* source) {
    remote_source_data* data = new remote_source_data();
    data->source = source;

    data->server_ip = obs_data_get_string(settings, "server_ip");
    data->server_port = (int)obs_data_get_int(settings, "server_port");
    // has_audio 從服務端獲取，不是本地設定
    
    obs_source_set_audio_mixers(source, 0x3F);
    obs_source_set_audio_active(source, false);  // 初始為 false，等待服務端通知
    remote_source_update(data, settings);

    return data;
}

static void remote_source_destroy(void* data_ptr) {
    remote_source_data* data = (remote_source_data*)data_ptr;

    stop_streaming(data);
    disconnect_and_release_session(data);

    // 清除 async video 輸出 (傳入 NULL 停用)
    obs_source_output_video(data->source, nullptr);

    data->h264_decoder.reset();

    delete data;
}

static void remote_source_update(void* data_ptr, obs_data_t* settings) {
    remote_source_data* data = (remote_source_data*)data_ptr;

    std::string new_ip = obs_data_get_string(settings, "server_ip");
    int new_port = (int)obs_data_get_int(settings, "server_port");
    std::string new_source_type = obs_data_get_string(settings, "source_type");

    bool reconnect = (new_ip != data->server_ip || new_port != data->server_port);

    // IP/Port 變更使用 debounce，等待 3 秒後才重新連線
    if (reconnect) {
        data->pending_ip = new_ip;
        data->pending_port = new_port;
        data->pending_connect_time = std::chrono::steady_clock::now();
        data->pending_connect = true;
        blog(LOG_INFO, "[Remote Source] Connection settings changed, will apply in 3s: %s:%d", 
             new_ip.c_str(), new_port);
        return;  // 延遲應用，在 video_tick 中檢查
    }
    
    // 連接並創建 session (如果需要)
    if (!data->connected.load() && !data->server_ip.empty()) {
        connect_and_create_session(data);
    }
    
    // 切換 source type (如果已連接且類型不同)
    if (data->connected.load() && !new_source_type.empty() && 
        new_source_type != data->current_source_type) {
        
        blog(LOG_INFO, "[Remote Source] Switching source type to %s", new_source_type.c_str());
        
        std::vector<GrpcClient::Property> new_props;
        if (data->grpc_client->setSourceType(data->session_id, new_source_type, new_props)) {
            data->current_source_type = new_source_type;
            
            std::lock_guard<std::mutex> lock(data->props_mutex);
            data->cached_props = std::move(new_props);
            
            blog(LOG_INFO, "[Remote Source] Source type set, got %zu properties", 
                 data->cached_props.size());
            
            // 延遲檢查音頻狀態
            schedule_audio_active_check(data);
        }
    }
    
    // 收集並應用 child_ 開頭的設定
    if (data->connected.load() && !data->current_source_type.empty()) {
        obs_data_t* settings_to_send = extract_child_settings(settings);
        
        const char* json_str = obs_data_get_json(settings_to_send);
        if (json_str) {
            std::vector<GrpcClient::Property> new_props;
            if (data->grpc_client->updateSettings(data->session_id, std::string(json_str), new_props)) {
                std::lock_guard<std::mutex> lock(data->props_mutex);
                data->cached_props = std::move(new_props);
                blog(LOG_INFO, "[Remote Source] Settings updated, refreshed %zu properties",
                        data->cached_props.size());
                
                // 設定更新後延遲檢查音頻狀態
                schedule_audio_active_check(data);
            }
        }
        obs_data_release(settings_to_send);
    }
    
    // 開始串流 (如果已設定 source type)
    if (data->connected.load() && !data->current_source_type.empty() && !data->streaming.load()) {
        start_streaming(data);
    }

    if (data->source) {
        obs_source_update_properties(data->source);
    }
}

static uint32_t remote_source_get_width(void* data_ptr) {
    remote_source_data* data = (remote_source_data*)data_ptr;
    return data->video_width.load();
}

static uint32_t remote_source_get_height(void* data_ptr) {
    remote_source_data* data = (remote_source_data*)data_ptr;
    return data->video_height.load();
}

// video_render 回調已移除 - 使用 OBS_SOURCE_ASYNC_VIDEO 模式
// OBS 會自動根據 obs_source_output_video() 的 timestamp 渲染視訊

static void remote_source_video_tick(void* data_ptr, float seconds) {
    UNUSED_PARAMETER(seconds);
    remote_source_data* data = (remote_source_data*)data_ptr;
    if (!data) return;
    
    // 檢查連線 debounce
    if (data->pending_connect) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - data->pending_connect_time).count();
        
        if (elapsed >= 1) {
            // 3秒已過，應用連線設定
            data->pending_connect = false;
            
            blog(LOG_INFO, "[Remote Source] Applying connection settings: %s:%d", 
                 data->pending_ip.c_str(), data->pending_port);
            
            stop_streaming(data);
            disconnect_and_release_session(data);
            data->server_ip = data->pending_ip;
            data->server_port = data->pending_port;
            
            // 嘗試連接
            if (!data->server_ip.empty()) {
                connect_and_create_session(data);
                if (data->source) {
                    obs_source_update_properties(data->source);
                }
            }
        }
    }
}

// ========== 屬性面板 Helper 函數 ==========
// 移除所有 child_ 開頭的屬性
static void remove_child_properties(obs_properties_t* props) {
    std::vector<std::string> to_remove;
    obs_property_t* prop = obs_properties_first(props);
    while (prop) {
        const char* name = obs_property_name(prop);
        if (strncmp(name, "child_", 6) == 0) {
            to_remove.push_back(name);
        }
        obs_property_next(&prop);
    }
    for (const auto& name : to_remove) {
        obs_properties_remove_by_name(props, name.c_str());
    }
}

// 從 cached_props 添加 child_ 屬性到 props（需在 props_mutex 鎖內調用或已持有鎖）
static void add_child_properties_from_cache(obs_properties_t* props, 
                                            const std::vector<GrpcClient::Property>& cached_props) {
    for (const auto& cprop : cached_props) {
        // 跳過不可見的屬性
        if (!cprop.visible) continue;
        
        std::string child_name = "child_" + cprop.name;
        obs_property_t* new_prop = nullptr;
        
        switch (cprop.type) {
        case OBS_PROPERTY_LIST: {
            // 根據 list_format 選擇正確的格式
            obs_combo_format format = (obs_combo_format)cprop.list_format;
            new_prop = obs_properties_add_list(props,
                child_name.c_str(), cprop.description.c_str(),
                OBS_COMBO_TYPE_LIST, format);
            
            for (const auto& item : cprop.items) {
                if (format == OBS_COMBO_FORMAT_INT) {
                    obs_property_list_add_int(new_prop, item.name.c_str(), item.value_int);
                } else if (format == OBS_COMBO_FORMAT_FLOAT) {
                    obs_property_list_add_float(new_prop, item.name.c_str(), item.value_float);
                } else {
                    obs_property_list_add_string(new_prop, item.name.c_str(), item.value_string.c_str());
                }
            }
            break;
        }
        case OBS_PROPERTY_BOOL:
            new_prop = obs_properties_add_bool(props, child_name.c_str(), cprop.description.c_str());
            break;
            
        case OBS_PROPERTY_INT: {
            // 根據 int_type 選擇 slider 或 spinbox
            if (cprop.int_type == 1) {  // OBS_NUMBER_SLIDER
                new_prop = obs_properties_add_int_slider(props, child_name.c_str(), 
                    cprop.description.c_str(), cprop.min_int, cprop.max_int, cprop.step_int);
            } else {
                new_prop = obs_properties_add_int(props, child_name.c_str(), 
                    cprop.description.c_str(), cprop.min_int, cprop.max_int, cprop.step_int);
            }
            break;
        }
        case OBS_PROPERTY_FLOAT: {
            // 根據 float_type 選擇 slider 或 spinbox
            if (cprop.float_type == 1) {  // OBS_NUMBER_SLIDER
                new_prop = obs_properties_add_float_slider(props, child_name.c_str(), 
                    cprop.description.c_str(), cprop.min_float, cprop.max_float, cprop.step_float);
            } else {
                new_prop = obs_properties_add_float(props, child_name.c_str(), 
                    cprop.description.c_str(), cprop.min_float, cprop.max_float, cprop.step_float);
            }
            break;
        }
        case OBS_PROPERTY_TEXT: {
            obs_text_type text_type = (obs_text_type)cprop.text_type;
            new_prop = obs_properties_add_text(props, child_name.c_str(), 
                cprop.description.c_str(), text_type);
            obs_property_text_set_info_type(new_prop, (obs_text_info_type)cprop.text_info_type);
            break;
        }
        case OBS_PROPERTY_PATH: {
            obs_path_type path_type = (obs_path_type)cprop.path_type;
            new_prop = obs_properties_add_path(props, child_name.c_str(), 
                cprop.description.c_str(), path_type, 
                cprop.path_filter.c_str(), cprop.path_default.c_str());
            break;
        }
        case OBS_PROPERTY_COLOR:
            new_prop = obs_properties_add_color(props, child_name.c_str(), cprop.description.c_str());
            break;
            
        case OBS_PROPERTY_COLOR_ALPHA:
            new_prop = obs_properties_add_color_alpha(props, child_name.c_str(), cprop.description.c_str());
            break;
            
        default:
            // 跳過不支援的類型 (BUTTON, FONT, etc.)
            blog(LOG_DEBUG, "[Remote Source] Skipping unsupported property type %d: %s", 
                 cprop.type, cprop.name.c_str());
            break;
        }
        
        // 設定 enabled 狀態和 long_description
        if (new_prop) {
            obs_property_set_enabled(new_prop, cprop.enabled);
            if (!cprop.long_description.empty()) {
                obs_property_set_long_description(new_prop, cprop.long_description.c_str());
            }
        }
    }
}

// ========== 屬性面板 ==========
static bool on_source_type_changed(obs_properties_t* props, obs_property_t* p,
                                    obs_data_t* settings) {
    UNUSED_PARAMETER(p);
    
    remote_source_data* data = (remote_source_data*)obs_properties_get_param(props);
    if (!data || !data->connected.load()) return false;
    
    const char* new_type = obs_data_get_string(settings, "source_type");
    if (!new_type || strlen(new_type) == 0) return false;
    if (data->current_source_type == new_type) return false;
    
    blog(LOG_INFO, "[Remote Source] Source type changed to %s", new_type);
    
    // 設定新的 source type
    std::vector<GrpcClient::Property> new_props;
    if (!data->grpc_client->setSourceType(data->session_id, new_type, new_props)) {
        return false;
    }
    
    data->current_source_type = new_type;
    
    // 延遲檢查音頻狀態
    schedule_audio_active_check(data);
    
    {
        std::lock_guard<std::mutex> lock(data->props_mutex);
        data->cached_props = std::move(new_props);
    }
    
    // 移除舊的 child_ 屬性並添加新的
    remove_child_properties(props);
    {
        std::lock_guard<std::mutex> lock(data->props_mutex);
        add_child_properties_from_cache(props, data->cached_props);
    }
    
    return true;
}

static obs_properties_t* remote_source_properties(void* data_ptr) {
    remote_source_data* data = (remote_source_data*)data_ptr;

    obs_properties_t* props = obs_properties_create();
    obs_properties_set_param(props, data, nullptr);

    // 伺服器設定
    obs_properties_add_text(props, "server_ip",
        "伺服器 IP (Server IP)", OBS_TEXT_DEFAULT);
    obs_properties_add_int(props, "server_port",
        "端口 (Port)", 1, 65535, 1);

    // 錯誤/狀態訊息顯示
    obs_property_t* error_prop = obs_properties_add_text(props, "error_message",
        "", OBS_TEXT_INFO);
    obs_property_set_enabled(error_prop, false);  // 只讀
    // 初始顯示錯誤狀態
    if (data) {
        std::string err = data->getError();
        if (!err.empty()) {
            obs_property_set_description(error_prop, ("⚠️ " + err).c_str());
        } else if (data->connected.load()) {
            obs_property_set_description(error_prop, "✅ Connected");
        } else {
            obs_property_set_visible(error_prop, false);
        }
    } else {
        obs_property_set_visible(error_prop, false);
    }

    // Source 類型選擇 (從 server 獲取)
    obs_property_t* source_type = obs_properties_add_list(props, "source_type",
        "來源類型 (Source Type)", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_set_modified_callback(source_type, on_source_type_changed);
    
    // 填充已有的 source 列表
    if (data) {
        for (const auto& src : data->available_sources) {
            obs_property_list_add_string(source_type, src.display_name.c_str(), src.id.c_str());
        }
    }
    
    // 動態添加 child_ 屬性 (如果已有緩存)
    if (data) {
        std::lock_guard<std::mutex> lock(data->props_mutex);
        add_child_properties_from_cache(props, data->cached_props);
    }

    // 音頻捕捉
    obs_properties_add_bool(props, "audio_capture",
        "音效捕捉 (Audio Capture) [實驗性]");

    return props;
}

static void remote_source_get_defaults(obs_data_t* settings) {
    obs_data_set_default_string(settings, "server_ip", "");
    obs_data_set_default_int(settings, "server_port", DEFAULT_SERVER_PORT);
    obs_data_set_default_string(settings, "source_type", "");
    obs_data_set_default_bool(settings, "audio_capture", false);
}

// ========== 來源信息結構 ==========
extern "C" {

struct obs_source_info remote_source_info;

void init_remote_source_info() {
    memset(&remote_source_info, 0, sizeof(remote_source_info));
    remote_source_info.id = "remote_source";
    remote_source_info.type = OBS_SOURCE_TYPE_INPUT;
    // OBS_SOURCE_ASYNC_VIDEO: 使用 obs_source_output_video() 輸出視訊
    // OBS 會自動根據 timestamp 同步音視訊
    remote_source_info.output_flags = OBS_SOURCE_AUDIO |
                                      OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
    remote_source_info.get_name = remote_source_get_name;
    remote_source_info.create = remote_source_create;
    remote_source_info.destroy = remote_source_destroy;
    remote_source_info.update = remote_source_update;
    // async video source 不需要 video_render，OBS 會自動處理
    // get_width/get_height 對 async source 也是可選的，但保留以供 UI 顯示
    remote_source_info.get_width = remote_source_get_width;
    remote_source_info.get_height = remote_source_get_height;
    remote_source_info.video_tick = remote_source_video_tick;
    remote_source_info.get_properties = remote_source_properties;
    remote_source_info.get_defaults = remote_source_get_defaults;
}

}  // extern "C"
