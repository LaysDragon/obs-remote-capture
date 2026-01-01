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

#include <thread>
#include <atomic>
#include <mutex>
#include <memory>
#include <functional>
#include <queue>

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReader;
using grpc::Status;
using namespace obsremote;

// ========== 客戶端類 ==========
class GrpcClient {
public:
    using VideoCallback = std::function<void(uint32_t width, uint32_t height, 
                                              const uint8_t* jpeg_data, size_t jpeg_size,
                                              uint64_t timestamp_ns)>;
    using AudioCallback = std::function<void(uint32_t sample_rate, uint32_t channels,
                                              const float* pcm_data, size_t samples,
                                              uint64_t timestamp_ns)>;

    GrpcClient(const std::string& server_address) 
        : channel_(grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials())),
          stub_(RemoteCaptureService::NewStub(channel_)),
          streaming_(false) {
        blog(LOG_INFO, "[gRPC Client] Created client for %s", server_address.c_str());
    }
    
    ~GrpcClient() {
        StopStream();
    }
    
    // 連接狀態
    bool IsConnected() {
        auto state = channel_->GetState(true);
        return state == GRPC_CHANNEL_READY || state == GRPC_CHANNEL_IDLE;
    }
    
    bool WaitForConnected(int timeout_ms = 5000) {
        auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(timeout_ms);
        return channel_->WaitForConnected(deadline);
    }
    
    // 獲取屬性列表
    bool GetProperties(const std::string& source_type, 
                       std::vector<Property>& out_properties) {
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
        
        out_properties.clear();
        for (const auto& prop : response.properties()) {
            out_properties.push_back(prop);
        }
        
        blog(LOG_INFO, "[gRPC Client] Got %zu properties", out_properties.size());
        return true;
    }
    
    // 開始接收串流
    bool StartStream(const std::string& source_type,
                     const std::map<std::string, std::string>& settings,
                     VideoCallback on_video,
                     AudioCallback on_audio) {
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
    VideoCallback on_video_;
    AudioCallback on_audio_;
};

// ========== C API 包裝器 ==========

struct grpc_client_handle {
    std::unique_ptr<GrpcClient> client;
    std::vector<Property> cached_properties;
};

extern "C" {

typedef struct grpc_client_handle* grpc_client_t;

grpc_client_t grpc_client_create(const char* server_address) {
    auto* handle = new grpc_client_handle();
    handle->client = std::make_unique<GrpcClient>(server_address);
    return handle;
}

void grpc_client_destroy(grpc_client_t client) {
    if (client) {
        delete client;
    }
}

bool grpc_client_is_connected(grpc_client_t client) {
    return client && client->client && client->client->IsConnected();
}

bool grpc_client_wait_connected(grpc_client_t client, int timeout_ms) {
    return client && client->client && client->client->WaitForConnected(timeout_ms);
}

// 獲取屬性
bool grpc_client_get_properties(grpc_client_t client, const char* source_type) {
    if (!client || !client->client) return false;
    return client->client->GetProperties(source_type, client->cached_properties);
}

size_t grpc_client_property_count(grpc_client_t client) {
    return client ? client->cached_properties.size() : 0;
}

const char* grpc_client_property_name(grpc_client_t client, size_t index) {
    if (!client || index >= client->cached_properties.size()) return nullptr;
    return client->cached_properties[index].name().c_str();
}

const char* grpc_client_property_description(grpc_client_t client, size_t index) {
    if (!client || index >= client->cached_properties.size()) return nullptr;
    return client->cached_properties[index].description().c_str();
}

int grpc_client_property_type(grpc_client_t client, size_t index) {
    if (!client || index >= client->cached_properties.size()) return 0;
    return client->cached_properties[index].type();
}

size_t grpc_client_property_item_count(grpc_client_t client, size_t index) {
    if (!client || index >= client->cached_properties.size()) return 0;
    return client->cached_properties[index].items_size();
}

const char* grpc_client_property_item_name(grpc_client_t client, size_t prop_index, size_t item_index) {
    if (!client || prop_index >= client->cached_properties.size()) return nullptr;
    const auto& prop = client->cached_properties[prop_index];
    if (item_index >= (size_t)prop.items_size()) return nullptr;
    return prop.items(static_cast<int>(item_index)).name().c_str();
}

const char* grpc_client_property_item_value(grpc_client_t client, size_t prop_index, size_t item_index) {
    if (!client || prop_index >= client->cached_properties.size()) return nullptr;
    const auto& prop = client->cached_properties[prop_index];
    if (item_index >= (size_t)prop.items_size()) return nullptr;
    return prop.items(static_cast<int>(item_index)).value().c_str();
}

// 串流回調類型
typedef void (*grpc_video_callback_t)(uint32_t width, uint32_t height,
                                       const uint8_t* jpeg_data, size_t jpeg_size,
                                       uint64_t timestamp_ns, void* user_data);
typedef void (*grpc_audio_callback_t)(uint32_t sample_rate, uint32_t channels,
                                       const float* pcm_data, size_t samples,
                                       uint64_t timestamp_ns, void* user_data);

struct stream_callback_data {
    grpc_video_callback_t video_cb;
    grpc_audio_callback_t audio_cb;
    void* user_data;
};

static thread_local stream_callback_data g_stream_callbacks;

bool grpc_client_start_stream(grpc_client_t client, const char* source_type,
                               const char** setting_keys, const char** setting_values, 
                               size_t settings_count,
                               grpc_video_callback_t on_video,
                               grpc_audio_callback_t on_audio,
                               void* user_data) {
    if (!client || !client->client) return false;
    
    std::map<std::string, std::string> settings;
    for (size_t i = 0; i < settings_count; i++) {
        settings[setting_keys[i]] = setting_values[i];
    }
    
    g_stream_callbacks.video_cb = on_video;
    g_stream_callbacks.audio_cb = on_audio;
    g_stream_callbacks.user_data = user_data;
    
    return client->client->StartStream(
        source_type, settings,
        [](uint32_t w, uint32_t h, const uint8_t* d, size_t s, uint64_t t) {
            if (g_stream_callbacks.video_cb) {
                g_stream_callbacks.video_cb(w, h, d, s, t, g_stream_callbacks.user_data);
            }
        },
        [](uint32_t sr, uint32_t ch, const float* d, size_t s, uint64_t t) {
            if (g_stream_callbacks.audio_cb) {
                g_stream_callbacks.audio_cb(sr, ch, d, s, t, g_stream_callbacks.user_data);
            }
        }
    );
}

void grpc_client_stop_stream(grpc_client_t client) {
    if (client && client->client) {
        client->client->StopStream();
    }
}

bool grpc_client_is_streaming(grpc_client_t client) {
    return client && client->client && client->client->IsStreaming();
}

}  // extern "C"

#endif  // HAVE_GRPC
