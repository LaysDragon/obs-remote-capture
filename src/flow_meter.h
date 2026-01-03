/*
 * flow_meter.h
 * 流量計 - 追蹤多個串流的數據傳輸速率
 */

#pragma once

#include <atomic>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <QString>
#include <obs-module.h>

class FlowMeter {
public:
    struct StreamMeter {
        std::atomic<uint64_t> bytes{0};
        uint64_t last_bytes{0};
        double rate_per_sec{0.0};
    };

    // 註冊新串流，返回唯一 ID
    uint64_t registerStream();

    // 註銷串流
    void unregisterStream(uint64_t id);

    // 添加字節數到指定串流
    void addBytes(uint64_t id, size_t bytes);

    // 獲取格式化的總速率字串
    QString getFormattedRate();

    // 獲取活躍串流數量
    size_t getActiveStreamCount();

    // 每秒調用一次更新速率
    void tick();

    // 獲取總速率 (bytes/sec)
    double getTotalRate() const { return total_rate_; }
    
    // 獲取指定串流的速率 (bytes/sec)
    double getStreamRate(uint64_t id);
    
    // 格式化速率字串 (靜態工具函數)
    static QString formatRate(double bytes_per_sec);

private:
    std::mutex mutex_;
    std::unordered_map<uint64_t, std::unique_ptr<StreamMeter>> streams_;
    std::atomic<uint64_t> next_id_{1};
    double total_rate_{0.0};
};

// 全局流量計實例
extern FlowMeter g_flow_meter;
