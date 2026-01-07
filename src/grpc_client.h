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
    using StreamInfoCallback = std::function<void(int64_t stream_start_time_ns)>;

    // Source 資訊
    struct SourceInfo {
        std::string id;
        std::string display_name;
    };
    
    // 屬性項目 (列表選項)
    struct PropertyItem {
        std::string name;
        std::string value_string;
        int64_t value_int = 0;
        double value_float = 0.0;
    };
    
    // 屬性結構
    struct Property {
        std::string name;
        std::string description;
        int type = 0;
        bool visible = true;
        bool enabled = true;
        std::string long_description;
        
        // LIST
        std::vector<PropertyItem> items;
        std::string current_string;
        int64_t current_int = 0;
        double current_float = 0.0;
        int list_format = 0;  // 0=string, 1=int, 2=float
        
        // Bool
        bool current_bool = false;
        
        // Int
        int min_int = 0, max_int = 0, step_int = 1;
        int current_int_value = 0;
        int int_type = 0;  // 0=scroller, 1=slider
        
        // Float
        double min_float = 0, max_float = 0, step_float = 0.01;
        double current_float_value = 0;
        int float_type = 0;  // 0=scroller, 1=slider
        
        // Text
        std::string current_text;
        int text_type = 0;       // 0=default, 1=password, 2=multiline, 3=info
        int text_info_type = 0;  // 0=normal, 1=warning, 2=error
        
        // Path
        int path_type = 0;  // 0=file, 1=file_save, 2=directory
        std::string path_filter;
        std::string path_default;
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
                        const std::string& settings_json,
                        std::vector<Property>& out_properties);
    
    // 獲取屬性
    bool getProperties(const std::string& session_id,
                       std::vector<Property>& out_properties);
    
    // 查詢音頻狀態 (供延遲調用)
    bool isAudioActive(const std::string& session_id, bool& out_audio_active);
    
    // SRT 連線資訊
    struct SrtInfo {
        int port = 0;
        int latency_ms = 200;
        std::string passphrase;
        bool ready = false;
    };
    
    // 獲取 SRT 串流資訊
    bool getSrtInfo(const std::string& session_id, SrtInfo& out_info);
    
    // 時鐘同步結果
    struct ClockSyncResult {
        int64_t clock_offset_ns = 0;  // Client 時間 - Server 時間 估算值
        int64_t rtt_ns = 0;           // 平均 RTT (納秒)
        bool valid = false;
    };
    
    // 時鐘同步 (執行多輪 RTT 測量，排除異常值)
    bool syncClock(ClockSyncResult& out_result, int rounds = 5);
    
    // 串流控制 (綁定 session_id)
    bool startStream(const std::string& session_id,
                     VideoCallback on_video,
                     AudioCallback on_audio,
                     StreamInfoCallback on_stream_info = nullptr);
    void stopStream();
    bool isStreaming() const;
    
    // 獲取時鐘同步資訊 (供外部使用)
    int64_t getClockOffset() const;
    int64_t getStreamStartTime() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif  // GRPC_CLIENT_H
