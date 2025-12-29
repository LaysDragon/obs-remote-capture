/*
 * audio-stream.cpp
 * 音訊處理輔助功能
 *
 * 提供:
 * 1. 音頻緩衝管理
 * 2. 重採樣支持 (如果需要)
 * 3. 音視頻同步
 */

#include <obs-module.h>
#include <util/circlebuf.h>
#include <util/threading.h>

#include <cstdint>
#include <cstring>
#include <atomic>
#include <mutex>

// ========== 音頻緩衝區 ==========
struct audio_buffer {
    struct circlebuf buffer;
    std::mutex mutex;

    uint32_t sample_rate;
    uint32_t channels;
    std::atomic<uint64_t> last_timestamp;

    audio_buffer() : sample_rate(48000), channels(2), last_timestamp(0) {
        circlebuf_init(&buffer);
    }

    ~audio_buffer() {
        circlebuf_free(&buffer);
    }
};

// ========== 初始化緩衝區 ==========
extern "C" audio_buffer* audio_buffer_create(uint32_t sample_rate, uint32_t channels) {
    audio_buffer* buf = new audio_buffer();
    buf->sample_rate = sample_rate;
    buf->channels = channels;
    return buf;
}

// ========== 銷毀緩衝區 ==========
extern "C" void audio_buffer_destroy(audio_buffer* buf) {
    if (buf) {
        delete buf;
    }
}

// ========== 寫入音頻數據 ==========
extern "C" void audio_buffer_push(audio_buffer* buf, const float* data,
                                  uint32_t samples, uint64_t timestamp) {
    if (!buf || !data || samples == 0) return;

    std::lock_guard<std::mutex> lock(buf->mutex);

    size_t data_size = samples * buf->channels * sizeof(float);
    circlebuf_push_back(&buf->buffer, data, data_size);
    buf->last_timestamp.store(timestamp);
}

// ========== 讀取音頻數據 ==========
extern "C" size_t audio_buffer_pop(audio_buffer* buf, float* data,
                                   uint32_t max_samples) {
    if (!buf || !data || max_samples == 0) return 0;

    std::lock_guard<std::mutex> lock(buf->mutex);

    size_t available = buf->buffer.size / (buf->channels * sizeof(float));
    size_t to_read = (available < max_samples) ? available : max_samples;

    if (to_read == 0) return 0;

    size_t data_size = to_read * buf->channels * sizeof(float);
    circlebuf_pop_front(&buf->buffer, data, data_size);

    return to_read;
}

// ========== 獲取緩衝區中的樣本數 ==========
extern "C" size_t audio_buffer_available(audio_buffer* buf) {
    if (!buf) return 0;

    std::lock_guard<std::mutex> lock(buf->mutex);
    return buf->buffer.size / (buf->channels * sizeof(float));
}

// ========== 清空緩衝區 ==========
extern "C" void audio_buffer_clear(audio_buffer* buf) {
    if (!buf) return;

    std::lock_guard<std::mutex> lock(buf->mutex);
    circlebuf_free(&buf->buffer);
    circlebuf_init(&buf->buffer);
}

// ========== 計算音視頻同步延遲 ==========
extern "C" int64_t audio_buffer_sync_offset(audio_buffer* buf,
                                            uint64_t video_timestamp) {
    if (!buf) return 0;

    uint64_t audio_ts = buf->last_timestamp.load();
    if (audio_ts == 0 || video_timestamp == 0) return 0;

    // 返回音頻相對於視頻的偏移 (毫秒)
    // 正值: 音頻超前, 負值: 音頻落後
    return (int64_t)(audio_ts - video_timestamp) / 1000000LL;
}

// ========== 簡單的線性插值重採樣 ==========
extern "C" void audio_resample_linear(const float* src, uint32_t src_samples,
                                      float* dst, uint32_t dst_samples,
                                      uint32_t channels) {
    if (src_samples == 0 || dst_samples == 0) return;

    double ratio = (double)(src_samples - 1) / (double)(dst_samples - 1);

    for (uint32_t i = 0; i < dst_samples; i++) {
        double src_pos = i * ratio;
        uint32_t idx = (uint32_t)src_pos;
        double frac = src_pos - idx;

        if (idx >= src_samples - 1) {
            // 最後一個樣本
            for (uint32_t c = 0; c < channels; c++) {
                dst[i * channels + c] = src[(src_samples - 1) * channels + c];
            }
        } else {
            // 線性插值
            for (uint32_t c = 0; c < channels; c++) {
                float s0 = src[idx * channels + c];
                float s1 = src[(idx + 1) * channels + c];
                dst[i * channels + c] = (float)(s0 + (s1 - s0) * frac);
            }
        }
    }
}

// ========== 音量調整 ==========
extern "C" void audio_apply_gain(float* data, uint32_t samples,
                                 uint32_t channels, float gain) {
    size_t total = samples * channels;
    for (size_t i = 0; i < total; i++) {
        data[i] *= gain;
    }
}

// ========== 交錯 -> 平面 (Interleaved to Planar) ==========
extern "C" void audio_deinterleave(const float* interleaved, float** planar,
                                   uint32_t samples, uint32_t channels) {
    for (uint32_t s = 0; s < samples; s++) {
        for (uint32_t c = 0; c < channels; c++) {
            planar[c][s] = interleaved[s * channels + c];
        }
    }
}

// ========== 平面 -> 交錯 (Planar to Interleaved) ==========
extern "C" void audio_interleave(const float** planar, float* interleaved,
                                 uint32_t samples, uint32_t channels) {
    for (uint32_t s = 0; s < samples; s++) {
        for (uint32_t c = 0; c < channels; c++) {
            interleaved[s * channels + c] = planar[c][s];
        }
    }
}
