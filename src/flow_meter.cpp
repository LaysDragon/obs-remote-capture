/*
 * flow_meter.cpp
 * 流量計實現
 */

#include "flow_meter.h"

// 全局流量計實例
FlowMeter g_flow_meter;

uint64_t FlowMeter::registerStream() {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t id = next_id_++;
    streams_[id] = std::make_unique<StreamMeter>();
    blog(LOG_INFO, "[FlowMeter] Registered stream %llu", (unsigned long long)id);
    return id;
}

void FlowMeter::unregisterStream(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    streams_.erase(id);
    blog(LOG_INFO, "[FlowMeter] Unregistered stream %llu, active streams: %zu", 
         (unsigned long long)id, streams_.size());
}

void FlowMeter::addBytes(uint64_t id, size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(id);
    if (it != streams_.end()) {
        it->second->bytes += bytes;
    }
}

QString FlowMeter::getFormattedRate() {
    return formatRate(total_rate_);
}

// 靜態工具函數：格式化速率
QString FlowMeter::formatRate(double bytes_per_sec) {
    if (bytes_per_sec >= 1024.0 * 1024.0) {
        return QString("%1 MB/s").arg(bytes_per_sec / (1024.0 * 1024.0), 0, 'f', 2);
    } else if (bytes_per_sec >= 1024.0) {
        return QString("%1 KB/s").arg(bytes_per_sec / 1024.0, 0, 'f', 2);
    } else {
        return QString("%1 B/s").arg(bytes_per_sec, 0, 'f', 0);
    }
}

size_t FlowMeter::getActiveStreamCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    return streams_.size();
}

// 獲取指定串流的速率
double FlowMeter::getStreamRate(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(id);
    if (it != streams_.end()) {
        return it->second->rate_per_sec;
    }
    return 0.0;
}

void FlowMeter::tick() {
    std::lock_guard<std::mutex> lock(mutex_);
    double total = 0.0;
    for (auto& pair : streams_) {
        StreamMeter* m = pair.second.get();
        uint64_t curr = m->bytes.load();
        uint64_t diff = curr - m->last_bytes;
        m->rate_per_sec = (double)diff;
        m->last_bytes = curr;
        total += m->rate_per_sec;
    }
    total_rate_ = total;
}

