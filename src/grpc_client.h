/*
 * grpc_client.h
 * gRPC 客戶端 C++ 類聲明
 */

#ifndef GRPC_CLIENT_H
#define GRPC_CLIENT_H

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <atomic>
#include <thread>

// Forward declarations
namespace grpc {
    class Channel;
    class ClientContext;
}

namespace obsremote {
    class Property;
    class RemoteCaptureService;
}

// ========== gRPC 客戶端類 ==========
class GrpcClient {
public:
    // 回調類型定義
    // VideoCallback: 接收視頻幀數據
    //   codec: 編碼類型 (與 VideoCodecType enum 對應)
    //   frame_data/frame_size: 編碼後的數據
    //   linesize: Raw 格式時的行寬 (JPEG 時為 0)
    using VideoCallback = std::function<void(uint32_t width, uint32_t height, 
                                              int codec,
                                              const uint8_t* frame_data, size_t frame_size,
                                              uint32_t linesize,
                                              uint64_t timestamp_ns)>;
    using AudioCallback = std::function<void(uint32_t sample_rate, uint32_t channels,
                                              const float* pcm_data, size_t samples,
                                              uint64_t timestamp_ns)>;

    // 屬性項目
    struct PropertyItem {
        std::string name;
        std::string value;
    };
    
    // 屬性結構
    struct Property {
        std::string name;
        std::string description;
        int type;
        bool visible;
        std::vector<PropertyItem> items;
    };

    // 構造/析構
    explicit GrpcClient(const std::string& server_address);
    ~GrpcClient();
    
    // 連接狀態
    bool isConnected();
    bool waitForConnected(int timeout_ms = 5000);
    
    // 獲取屬性列表
    bool getProperties(const std::string& source_type);
    const std::vector<Property>& getCachedProperties() const { return cached_properties_; }
    
    // 串流控制
    bool startStream(const std::string& source_type,
                     const std::map<std::string, std::string>& settings,
                     VideoCallback on_video,
                     AudioCallback on_audio);
    void stopStream();
    bool isStreaming() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<Property> cached_properties_;
};

#endif  // GRPC_CLIENT_H
