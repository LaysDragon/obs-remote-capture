/*
 * grpc_client.h
 * gRPC 客戶端 - Session-based API
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
    using VideoCallback = std::function<void(uint32_t width, uint32_t height, 
                                              int codec,
                                              const uint8_t* frame_data, size_t frame_size,
                                              uint32_t linesize,
                                              uint64_t timestamp_ns)>;
    using AudioCallback = std::function<void(uint32_t sample_rate, uint32_t channels,
                                              const float* pcm_data, size_t samples,
                                              uint64_t timestamp_ns)>;

    // Source 資訊
    struct SourceInfo {
        std::string id;
        std::string display_name;
    };
    
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
        std::string current_string;
        
        // Bool
        bool default_bool;
        
        // Int
        int min_int, max_int, step_int, default_int;
        
        // Float
        double min_float, max_float, step_float, default_float;
    };

    // 構造/析構
    explicit GrpcClient(const std::string& server_address);
    ~GrpcClient();
    
    // 連接狀態
    bool isConnected();
    bool waitForConnected(int timeout_ms = 5000);
    
    // ========== Session-based API ==========
    
    // 獲取可用 source 類型列表
    bool getAvailableSources(std::vector<SourceInfo>& out);
    
    // 創建 Session
    bool createSession(std::string& out_session_id);
    
    // 釋放 Session
    bool releaseSession(const std::string& session_id);
    
    // 設定 Source Type (返回新 properties)
    bool setSourceType(const std::string& session_id, 
                       const std::string& source_type,
                       std::vector<Property>& out_properties);
    
    // 更新設定 (返回刷新後的 properties)
    bool updateSettings(const std::string& session_id,
                        const std::map<std::string, std::string>& settings,
                        std::vector<Property>& out_properties);
    
    // 獲取屬性
    bool getProperties(const std::string& session_id,
                       std::vector<Property>& out_properties);
    
    // 串流控制 (綁定 session_id)
    bool startStream(const std::string& session_id,
                     VideoCallback on_video,
                     AudioCallback on_audio);
    void stopStream();
    bool isStreaming() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif  // GRPC_CLIENT_H
