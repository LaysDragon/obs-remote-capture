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
                       std::vector<GrpcClient::Property>& out_properties,
                       bool& out_has_audio) {
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
        out_has_audio = response.has_audio();
        blog(LOG_INFO, "[gRPC Client] SetSourceType: %s, got %zu properties, has_audio=%d", 
             source_type.c_str(), out_properties.size(), out_has_audio);
        return true;
    }
    
    // 更新設定
    bool UpdateSettings(const std::string& session_id,
                        const std::string& settings_json,
                        std::vector<GrpcClient::Property>& out_properties,
                        bool& out_has_audio) {
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
        out_has_audio = response.has_audio();
        blog(LOG_INFO, "[gRPC Client] UpdateSettings: got %zu refreshed properties, has_audio=%d", 
             out_properties.size(), out_has_audio);
        return true;
    }
    
    // 獲取屬性
    bool GetProperties(const std::string& session_id,
                       std::vector<GrpcClient::Property>& out_properties,
                       bool& out_has_audio) {
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
        out_has_audio = response.has_audio();
        blog(LOG_INFO, "[gRPC Client] GetProperties: got %zu properties, has_audio=%d", 
             out_properties.size(), out_has_audio);
        return true;
    }
    
    // 開始接收串流
    bool StartStream(const std::string& session_id,
                     GrpcClient::VideoCallback on_video,
                     GrpcClient::AudioCallback on_audio) {
        if (streaming_.load()) {
            StopStream();
        }
        
        streaming_.store(true);
        on_video_ = on_video;
        on_audio_ = on_audio;
        
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
                if (frame.has_video()) {
                    const VideoFrame& v = frame.video();
                    video_frame_count++;
                    
                    if (video_frame_count % 100 == 1) {
                        blog(LOG_INFO, "[gRPC Client] Video frame #%u: %ux%u, codec=%d, size=%zu",
                             video_frame_count, v.width(), v.height(), 
                             v.codec(), v.frame_data().size());
                    }
                    
                    if (on_video_) {
                        on_video_(v.width(), v.height(),
                                  static_cast<int>(v.codec()),
                                  reinterpret_cast<const uint8_t*>(v.frame_data().data()),
                                  v.frame_data().size(),
                                  v.linesize(),
                                  v.timestamp_ns());
                    }
                } else if (frame.has_audio()) {
                    const AudioFrame& a = frame.audio();
                    audio_frame_count++;
                    
                    if (on_audio_) {
                        // frame_data 現在是 planar 格式 (L|L|L|...|R|R|R|...)
                        // 每個 channel 的樣本數 = 總大小 / sizeof(float) / 2
                        size_t samples_per_channel = a.frame_data().size() / sizeof(float) / 2;
                        on_audio_(a.sample_rate(), a.channels(),
                                  reinterpret_cast<const float*>(a.frame_data().data()),
                                  samples_per_channel,
                                  a.timestamp_ns());
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
                                std::vector<Property>& out_properties,
                                bool& out_has_audio) {
    return impl_->SetSourceType(session_id, source_type, out_properties, out_has_audio);
}

bool GrpcClient::updateSettings(const std::string& session_id,
                                 const std::string& settings_json,
                                 std::vector<Property>& out_properties,
                                 bool& out_has_audio) {
    return impl_->UpdateSettings(session_id, settings_json, out_properties, out_has_audio);
}

bool GrpcClient::getProperties(const std::string& session_id,
                                std::vector<Property>& out_properties,
                                bool& out_has_audio) {
    return impl_->GetProperties(session_id, out_properties, out_has_audio);
}

bool GrpcClient::startStream(const std::string& session_id,
                              VideoCallback on_video,
                              AudioCallback on_audio) {
    return impl_->StartStream(session_id, on_video, on_audio);
}

void GrpcClient::stopStream() {
    impl_->StopStream();
}

bool GrpcClient::isStreaming() const {
    return impl_->IsStreaming();
}
