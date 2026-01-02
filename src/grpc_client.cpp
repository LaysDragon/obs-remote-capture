/*
 * grpc_client.cpp
 * gRPC 客戶端實現
 *
 * 功能:
 * 1. 調用 GetProperties - 獲取遠端捕捉源屬性
 * 2. 調用 StartStream - 接收影音串流
 * 3. 調用 StopStream - 停止串流
 */

#ifdef HAVE_GRPC

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

// ========== PIMPL 實現類 ==========
class GrpcClient::Impl {
public:
    Impl(const std::string& server_address) 
        : channel_(grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials())),
          stub_(RemoteCaptureService::NewStub(channel_)),
          streaming_(false) {
        blog(LOG_INFO, "[gRPC Client] Created client for %s", server_address.c_str());
    }
    
    ~Impl() {
        StopStream();
    }
    
    // 連接狀態
    bool IsConnected() {
        auto state = channel_->GetState(true);
        return state == GRPC_CHANNEL_READY || state == GRPC_CHANNEL_IDLE;
    }
    
    bool WaitForConnected(int timeout_ms) {
        auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(timeout_ms);
        return channel_->WaitForConnected(deadline);
    }
    
    // 獲取屬性列表
    bool GetProperties(const std::string& source_type, std::vector<GrpcClient::Property>& out) {
        GetPropertiesRequest request;
        request.set_source_type(source_type);
        
        GetPropertiesResponse response;
        ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        
        Status status = stub_->GetProperties(&context, request, &response);
        
        if (!status.ok()) {
            blog(LOG_WARNING, "[gRPC Client] GetProperties failed: %s", 
                 status.error_message().c_str());
            return false;
        }
        
        out.clear();
        for (const auto& prop : response.properties()) {
            GrpcClient::Property p;
            p.name = prop.name();
            p.description = prop.description();
            p.type = prop.type();
            p.visible = prop.visible();
            
            for (const auto& item : prop.items()) {
                p.items.push_back({item.name(), item.value()});
            }
            
            out.push_back(std::move(p));
        }
        
        blog(LOG_INFO, "[gRPC Client] Got %zu properties", out.size());
        return true;
    }
    
    // 開始接收串流
    bool StartStream(const std::string& source_type,
                     const std::map<std::string, std::string>& settings,
                     GrpcClient::VideoCallback on_video,
                     GrpcClient::AudioCallback on_audio) {
        if (streaming_.load()) {
            StopStream();
        }
        
        streaming_.store(true);
        on_video_ = on_video;
        on_audio_ = on_audio;
        
        stream_thread_ = std::thread([this, source_type, settings]() {
            StartStreamRequest request;
            request.set_source_type(source_type);
            for (const auto& pair : settings) {
                (*request.mutable_settings())[pair.first] = pair.second;
            }
            
            stream_context_ = std::make_unique<ClientContext>();
            std::unique_ptr<ClientReader<StreamFrame>> reader = 
                stub_->StartStream(stream_context_.get(), request);
            
            blog(LOG_INFO, "[gRPC Client] Stream started");
            
            StreamFrame frame;
            while (streaming_.load() && reader->Read(&frame)) {
                if (frame.has_video()) {
                    const VideoFrame& v = frame.video();
                    if (on_video_) {
                        on_video_(v.width(), v.height(),
                                  reinterpret_cast<const uint8_t*>(v.jpeg_data().data()),
                                  v.jpeg_data().size(),
                                  v.timestamp_ns());
                    }
                } else if (frame.has_audio()) {
                    const AudioFrame& a = frame.audio();
                    if (on_audio_) {
                        on_audio_(a.sample_rate(), a.channels(),
                                  reinterpret_cast<const float*>(a.pcm_data().data()),
                                  a.pcm_data().size() / sizeof(float) / a.channels(),
                                  a.timestamp_ns());
                    }
                }
            }
            
            Status status = reader->Finish();
            if (!status.ok() && streaming_.load()) {
                blog(LOG_WARNING, "[gRPC Client] Stream ended with error: %s",
                     status.error_message().c_str());
            }
            
            blog(LOG_INFO, "[gRPC Client] Stream stopped");
        });
        
        return true;
    }
    
    // 停止串流
    void StopStream() {
        if (!streaming_.load()) return;
        
        streaming_.store(false);
        
        // 取消 RPC
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

bool GrpcClient::getProperties(const std::string& source_type) {
    return impl_->GetProperties(source_type, cached_properties_);
}

bool GrpcClient::startStream(const std::string& source_type,
                              const std::map<std::string, std::string>& settings,
                              VideoCallback on_video,
                              AudioCallback on_audio) {
    return impl_->StartStream(source_type, settings, on_video, on_audio);
}

void GrpcClient::stopStream() {
    impl_->StopStream();
}

bool GrpcClient::isStreaming() const {
    return impl_->IsStreaming();
}

#endif  // HAVE_GRPC
