/*
 * grpc_client.cpp
 * gRPC 客戶端實現 - Session-based API
 *
 * 功能:
 * 1. GetAvailableSources - 獲取可用 source 列表
 * 2. CreateSession/ReleaseSession - Session 管理
 * 3. SetSourceType - 切換 source 類型
 * 4. UpdateSettings - 更新設定並獲取刷新屬性
 * 5. StartStream - 綁定 session_id 接收串流
 */

#include "grpc_client.h"

#include <obs-module.h>
#include <util/platform.h>

// 禁用 protobuf 生成代碼的警告
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4267)  // size_t to int conversion
#pragma warning(disable: 4244)  // possible loss of data
#endif

#include <grpcpp/grpcpp.h>
#include "remote_capture.grpc.pb.h"

#ifdef _MSC_VER
#pragma warning(pop)
#endif

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReader;
using grpc::Status;
using namespace obsremote;

// ========== 從 proto Property 轉換 ==========
static void convert_property(const obsremote::Property& src, GrpcClient::Property& dst) {
    dst.name = src.name();
    dst.description = src.description();
    dst.type = src.type();
    dst.visible = src.visible();
    dst.current_string = src.current_string();
    dst.default_bool = src.default_bool();
    dst.min_int = src.min_int();
    dst.max_int = src.max_int();
    dst.step_int = src.step_int();
    dst.default_int = src.default_int();
    dst.min_float = src.min_float();
    dst.max_float = src.max_float();
    dst.step_float = src.step_float();
    dst.default_float = src.default_float();
    
    dst.items.clear();
    for (const auto& item : src.items()) {
        dst.items.push_back({item.name(), item.value()});
    }
}

static void convert_properties(const google::protobuf::RepeatedPtrField<obsremote::Property>& src,
                               std::vector<GrpcClient::Property>& dst) {
    dst.clear();
    for (const auto& prop : src) {
        GrpcClient::Property p;
        convert_property(prop, p);
        dst.push_back(std::move(p));
    }
}

// ========== PIMPL 實現類 ==========
class GrpcClient::Impl {
public:
    Impl(const std::string& server_address) 
        : streaming_(false) {
        grpc::ChannelArguments args;
        args.SetMaxReceiveMessageSize(100 * 1024 * 1024);
        args.SetMaxSendMessageSize(100 * 1024 * 1024);
        
        channel_ = grpc::CreateCustomChannel(
            server_address, grpc::InsecureChannelCredentials(), args);
        stub_ = RemoteCaptureService::NewStub(channel_);
        
        blog(LOG_INFO, "[gRPC Client] Created client for %s", server_address.c_str());
    }
    
    ~Impl() {
        StopStream();
    }
    
    bool IsConnected() {
        auto state = channel_->GetState(true);
        return state == GRPC_CHANNEL_READY || state == GRPC_CHANNEL_IDLE;
    }
    
    bool WaitForConnected(int timeout_ms) {
        auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(timeout_ms);
        return channel_->WaitForConnected(deadline);
    }
    
    // 獲取可用 source 列表
    bool GetAvailableSources(std::vector<GrpcClient::SourceInfo>& out) {
        Empty request;
        AvailableSourcesResponse response;
        ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        
        Status status = stub_->GetAvailableSources(&context, request, &response);
        if (!status.ok()) {
            blog(LOG_WARNING, "[gRPC Client] GetAvailableSources failed: %s", 
                 status.error_message().c_str());
            return false;
        }
        
        out.clear();
        for (const auto& src : response.sources()) {
            out.push_back({src.id(), src.display_name()});
        }
        
        blog(LOG_INFO, "[gRPC Client] Got %zu available sources", out.size());
        return true;
    }
    
    // 創建 Session
    bool CreateSession(std::string& out_session_id) {
        Empty request;
        CreateSessionResponse response;
        ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        
        Status status = stub_->CreateSession(&context, request, &response);
        if (!status.ok()) {
            blog(LOG_WARNING, "[gRPC Client] CreateSession failed: %s", 
                 status.error_message().c_str());
            return false;
        }
        
        out_session_id = response.session_id();
        blog(LOG_INFO, "[gRPC Client] Created session: %s", out_session_id.c_str());
        return true;
    }
    
    // 釋放 Session
    bool ReleaseSession(const std::string& session_id) {
        ReleaseSessionRequest request;
        request.set_session_id(session_id);
        
        Empty response;
        ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        
        Status status = stub_->ReleaseSession(&context, request, &response);
        if (!status.ok()) {
            blog(LOG_WARNING, "[gRPC Client] ReleaseSession failed: %s", 
                 status.error_message().c_str());
            return false;
        }
        
        blog(LOG_INFO, "[gRPC Client] Released session: %s", session_id.c_str());
        return true;
    }
    
    // 設定 Source Type
    bool SetSourceType(const std::string& session_id,
                       const std::string& source_type,
                       std::vector<GrpcClient::Property>& out_properties) {
        SetSourceTypeRequest request;
        request.set_session_id(session_id);
        request.set_source_type(source_type);
        
        SetSourceTypeResponse response;
        ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        
        Status status = stub_->SetSourceType(&context, request, &response);
        if (!status.ok()) {
            blog(LOG_WARNING, "[gRPC Client] SetSourceType failed: %s", 
                 status.error_message().c_str());
            return false;
        }
        
        convert_properties(response.properties(), out_properties);
        blog(LOG_INFO, "[gRPC Client] SetSourceType: %s, got %zu properties", 
             source_type.c_str(), out_properties.size());
        return true;
    }
    
    // 更新設定
    bool UpdateSettings(const std::string& session_id,
                        const std::string& settings_json,
                        std::vector<GrpcClient::Property>& out_properties) {
        UpdateSettingsRequest request;
        request.set_session_id(session_id);
        request.set_settings_json(settings_json);
        
        UpdateSettingsResponse response;
        ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        
        Status status = stub_->UpdateSettings(&context, request, &response);
        if (!status.ok()) {
            blog(LOG_WARNING, "[gRPC Client] UpdateSettings failed: %s", 
                 status.error_message().c_str());
            return false;
        }
        
        convert_properties(response.properties(), out_properties);
        blog(LOG_INFO, "[gRPC Client] UpdateSettings: got %zu refreshed properties", 
             out_properties.size());
        return true;
    }
    
    // 獲取屬性
    bool GetProperties(const std::string& session_id,
                       std::vector<GrpcClient::Property>& out_properties) {
        GetPropertiesRequest request;
        request.set_session_id(session_id);
        
        GetPropertiesResponse response;
        ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        
        Status status = stub_->GetProperties(&context, request, &response);
        if (!status.ok()) {
            blog(LOG_WARNING, "[gRPC Client] GetProperties failed: %s", 
                 status.error_message().c_str());
            return false;
        }
        
        convert_properties(response.properties(), out_properties);
        blog(LOG_INFO, "[gRPC Client] GetProperties: got %zu properties", 
             out_properties.size());
        return true;
    }
    
    // 查詢音頻狀態
    bool IsAudioActive(const std::string& session_id, bool& out_audio_active) {
        IsAudioActiveRequest request;
        request.set_session_id(session_id);
        
        IsAudioActiveResponse response;
        ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        
        Status status = stub_->IsAudioActive(&context, request, &response);
        if (!status.ok()) {
            blog(LOG_WARNING, "[gRPC Client] IsAudioActive failed: %s", 
                 status.error_message().c_str());
            return false;
        }
        
        out_audio_active = response.audio_active();
        blog(LOG_INFO, "[gRPC Client] IsAudioActive: session=%s, audio_active=%d", 
             session_id.c_str(), out_audio_active);
        return true;
    }
    
    // 獲取 SRT 串流資訊
    bool GetSrtInfo(const std::string& session_id, GrpcClient::SrtInfo& out_info) {
        GetSrtInfoRequest request;
        request.set_session_id(session_id);
        
        obsremote::SrtInfo response;
        ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        
        Status status = stub_->GetSrtInfo(&context, request, &response);
        if (!status.ok()) {
            blog(LOG_WARNING, "[gRPC Client] GetSrtInfo failed: %s", 
                 status.error_message().c_str());
            return false;
        }
        
        out_info.port = response.port();
        out_info.latency_ms = response.latency_ms();
        out_info.passphrase = response.passphrase();
        out_info.ready = response.ready();
        
        blog(LOG_INFO, "[gRPC Client] GetSrtInfo: session=%s, port=%d, ready=%d", 
             session_id.c_str(), out_info.port, out_info.ready);
        return true;
    }
    
    // 時鐘同步 (RTT 測量)
    bool SyncClock(GrpcClient::ClockSyncResult& out_result, int rounds) {
        std::vector<int64_t> rtts;
        std::vector<int64_t> offsets;
        
        for (int i = 0; i < rounds; i++) {
            SyncClockRequest request;
            request.set_client_send_time_ns(os_gettime_ns());
            
            int64_t send_time = os_gettime_ns();
            
            SyncClockResponse response;
            ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
            
            Status status = stub_->SyncClock(&context, request, &response);
            
            int64_t recv_time = os_gettime_ns();
            
            if (!status.ok()) {
                blog(LOG_WARNING, "[gRPC Client] SyncClock round %d failed: %s", 
                     i, status.error_message().c_str());
                continue;
            }
            
            int64_t rtt = recv_time - send_time;
            // offset = (client_mid_time) - server_time
            // client_mid_time = send_time + rtt/2
            int64_t client_mid = send_time + rtt / 2;
            int64_t offset = client_mid - response.server_time_ns();
            
            rtts.push_back(rtt);
            offsets.push_back(offset);
        }
        
        if (rtts.size() < 3) {
            blog(LOG_WARNING, "[gRPC Client] SyncClock: not enough successful rounds (%zu/%d)",
                 rtts.size(), rounds);
            out_result.valid = false;
            return false;
        }
        
        // 排除最大和最小 RTT 的異常值
        size_t min_idx = 0, max_idx = 0;
        for (size_t i = 1; i < rtts.size(); i++) {
            if (rtts[i] < rtts[min_idx]) min_idx = i;
            if (rtts[i] > rtts[max_idx]) max_idx = i;
        }
        
        int64_t sum_rtt = 0, sum_offset = 0;
        int count = 0;
        for (size_t i = 0; i < rtts.size(); i++) {
            if (i != min_idx && i != max_idx) {
                sum_rtt += rtts[i];
                sum_offset += offsets[i];
                count++;
            }
        }
        
        if (count == 0) {
            // 如果只有 2 個樣本，直接平均
            for (size_t i = 0; i < rtts.size(); i++) {
                sum_rtt += rtts[i];
                sum_offset += offsets[i];
            }
            count = (int)rtts.size();
        }
        
        out_result.rtt_ns = sum_rtt / count;
        out_result.clock_offset_ns = sum_offset / count;
        out_result.valid = true;
        
        blog(LOG_INFO, "[gRPC Client] SyncClock: offset=%lld ns (%.2f ms), avg_rtt=%.2f ms",
             (long long)out_result.clock_offset_ns, 
             out_result.clock_offset_ns / 1000000.0,
             out_result.rtt_ns / 1000000.0);
        
        // 存儲 offset 供後續 timestamp 轉換使用
        clock_offset_ns_ = out_result.clock_offset_ns;
        clock_sync_valid_ = true;
        
        return true;
    }
    
    // 開始接收串流
    bool StartStream(const std::string& session_id,
                     GrpcClient::VideoCallback on_video,
                     GrpcClient::AudioCallback on_audio,
                     GrpcClient::StreamInfoCallback on_stream_info) {
        if (streaming_.load()) {
            StopStream();
        }
        
        streaming_.store(true);
        on_video_ = on_video;
        on_audio_ = on_audio;
        on_stream_info_ = on_stream_info;
        
        stream_thread_ = std::thread([this, session_id]() {
            StartStreamRequest request;
            request.set_session_id(session_id);
            
            stream_context_ = std::make_unique<ClientContext>();
            std::unique_ptr<ClientReader<StreamFrame>> reader = 
                stub_->StartStream(stream_context_.get(), request);
            
            blog(LOG_INFO, "[gRPC Client] Stream started for session %s", session_id.c_str());
            
            StreamFrame frame;
            uint32_t video_frame_count = 0;
            uint32_t audio_frame_count = 0;
            
            while (streaming_.load() && reader->Read(&frame)) {
                if (frame.has_stream_info()) {
                    // 收到 StreamInfo - 僅在串流開始時發送一次
                    const StreamInfo& info = frame.stream_info();
                    stream_start_time_ns_ = info.stream_start_time_ns();
                    stream_info_received_ = true;
                    blog(LOG_INFO, "[gRPC Client] Received StreamInfo: stream_start_time_ns=%lld",
                         (long long)stream_start_time_ns_.load());
                    
                    // 回調通知外部
                    if (on_stream_info_) {
                        on_stream_info_(stream_start_time_ns_.load());
                    }
                } else if (frame.has_video()) {
                    const VideoFrame& v = frame.video();
                    video_frame_count++;
                    
                    if (video_frame_count % 100 == 1) {
                        blog(LOG_INFO, "[gRPC Client] Video frame #%u: %ux%u, codec=%d, size=%zu",
                             video_frame_count, v.width(), v.height(), 
                             v.codec(), v.frame_data().size());
                    }
                    
                    if (on_video_) {
                        // 將 server 時間戳轉換為 client 時間參考
                        // 1. 還原 server 捕獲時間: server_capture_time = stream_start_time + relative_pts
                        // 2. 轉換到 client 時間: client_capture_time = server_capture_time + clock_offset
                        uint64_t client_timestamp_ns = v.timestamp_ns();
                        if (stream_info_received_.load() && clock_sync_valid_.load()) {
                            int64_t relative_pts_ns = (int64_t)v.timestamp_ns();
                            int64_t server_capture_time_ns = stream_start_time_ns_.load() + relative_pts_ns;
                            client_timestamp_ns = (uint64_t)(server_capture_time_ns + clock_offset_ns_.load());
                        }
                        
                        on_video_(v.width(), v.height(),
                                  static_cast<int>(v.codec()),
                                  reinterpret_cast<const uint8_t*>(v.frame_data().data()),
                                  v.frame_data().size(),
                                  v.linesize(),
                                  client_timestamp_ns);
                    }
                } else if (frame.has_audio()) {
                    const AudioFrame& a = frame.audio();
                    audio_frame_count++;
                    
                    if (on_audio_) {
                        // frame_data 現在是 planar 格式 (L|L|L|...|R|R|R|...)
                        // 每個 channel 的樣本數 = 總大小 / sizeof(float) / 2
                        size_t samples_per_channel = a.frame_data().size() / sizeof(float) / 2;
                        
                        // 將 server 時間戳轉換為 client 時間參考
                        // 1. 還原 server 捕獲時間: server_capture_time = stream_start_time + relative_pts
                        // 2. 轉換到 client 時間: client_capture_time = server_capture_time + clock_offset
                        uint64_t client_timestamp_ns = a.timestamp_ns();
                        if (stream_info_received_.load() && clock_sync_valid_.load()) {
                            int64_t relative_pts_ns = (int64_t)a.timestamp_ns();
                            int64_t server_capture_time_ns = stream_start_time_ns_.load() + relative_pts_ns;
                            client_timestamp_ns = (uint64_t)(server_capture_time_ns + clock_offset_ns_.load());
                        }
                        
                        on_audio_(a.sample_rate(), a.channels(),
                                  reinterpret_cast<const float*>(a.frame_data().data()),
                                  samples_per_channel,
                                  client_timestamp_ns);
                    }
                }
            }
            
            Status status = reader->Finish();
            if (!status.ok() && streaming_.load()) {
                blog(LOG_WARNING, "[gRPC Client] Stream ended with error: %s",
                     status.error_message().c_str());
            }
            
            blog(LOG_INFO, "[gRPC Client] Stream stopped. video=%u, audio=%u",
                 video_frame_count, audio_frame_count);
        });
        
        return true;
    }
    
    void StopStream() {
        if (!streaming_.load()) return;
        
        streaming_.store(false);
        
        if (stream_context_) {
            stream_context_->TryCancel();
        }
        
        if (stream_thread_.joinable()) {
            stream_thread_.join();
        }
        
        stream_context_.reset();
    }
    
    bool IsStreaming() const {
        return streaming_.load();
    }
    
private:
    std::shared_ptr<Channel> channel_;
    std::unique_ptr<RemoteCaptureService::Stub> stub_;
    std::atomic<bool> streaming_;
    std::thread stream_thread_;
    std::unique_ptr<ClientContext> stream_context_;
    GrpcClient::VideoCallback on_video_;
    GrpcClient::AudioCallback on_audio_;
    GrpcClient::StreamInfoCallback on_stream_info_;
    
    // 時鐘同步資訊
    std::atomic<int64_t> clock_offset_ns_{0};  // client_time - server_time
    std::atomic<bool> clock_sync_valid_{false};
    
    // 串流開始時間 (Server 端的 os_gettime_ns)
    std::atomic<int64_t> stream_start_time_ns_{0};
    std::atomic<bool> stream_info_received_{false};
    
public:
    int64_t GetClockOffset() const { return clock_offset_ns_.load(); }
    int64_t GetStreamStartTime() const { return stream_start_time_ns_.load(); }
};

// ========== GrpcClient 公開接口實現 ==========

GrpcClient::GrpcClient(const std::string& server_address)
    : impl_(std::make_unique<Impl>(server_address)) {
}

GrpcClient::~GrpcClient() = default;

bool GrpcClient::isConnected() {
    return impl_->IsConnected();
}

bool GrpcClient::waitForConnected(int timeout_ms) {
    return impl_->WaitForConnected(timeout_ms);
}

bool GrpcClient::getAvailableSources(std::vector<SourceInfo>& out) {
    return impl_->GetAvailableSources(out);
}

bool GrpcClient::createSession(std::string& out_session_id) {
    return impl_->CreateSession(out_session_id);
}

bool GrpcClient::releaseSession(const std::string& session_id) {
    return impl_->ReleaseSession(session_id);
}

bool GrpcClient::setSourceType(const std::string& session_id,
                                const std::string& source_type,
                                std::vector<Property>& out_properties) {
    return impl_->SetSourceType(session_id, source_type, out_properties);
}

bool GrpcClient::updateSettings(const std::string& session_id,
                                 const std::string& settings_json,
                                 std::vector<Property>& out_properties) {
    return impl_->UpdateSettings(session_id, settings_json, out_properties);
}

bool GrpcClient::getProperties(const std::string& session_id,
                                std::vector<Property>& out_properties) {
    return impl_->GetProperties(session_id, out_properties);
}

bool GrpcClient::isAudioActive(const std::string& session_id, bool& out_audio_active) {
    return impl_->IsAudioActive(session_id, out_audio_active);
}

bool GrpcClient::getSrtInfo(const std::string& session_id, SrtInfo& out_info) {
    return impl_->GetSrtInfo(session_id, out_info);
}

bool GrpcClient::syncClock(ClockSyncResult& out_result, int rounds) {
    return impl_->SyncClock(out_result, rounds);
}

bool GrpcClient::startStream(const std::string& session_id,
                              VideoCallback on_video,
                              AudioCallback on_audio,
                              StreamInfoCallback on_stream_info) {
    return impl_->StartStream(session_id, on_video, on_audio, on_stream_info);
}

void GrpcClient::stopStream() {
    impl_->StopStream();
}

bool GrpcClient::isStreaming() const {
    return impl_->IsStreaming();
}

int64_t GrpcClient::getClockOffset() const {
    return impl_->GetClockOffset();
}

int64_t GrpcClient::getStreamStartTime() const {
    return impl_->GetStreamStartTime();
}
