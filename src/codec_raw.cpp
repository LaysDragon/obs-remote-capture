/*
 * codec_raw.cpp
 * Raw BGRA 編解碼器實現 (無壓縮)
 */

#include "video_codec.h"
#include <obs-module.h>
#include <cstring>

// ========== Raw 編碼器 ==========
class RawEncoder : public IVideoEncoder {
public:
    VideoCodecType getCodecType() const override {
        return VideoCodecType::RAW_BGRA;
    }
    
    const char* getName() const override {
        return "Raw BGRA";
    }
    
    bool encode(const uint8_t* bgra_data, 
                uint32_t width, uint32_t height, uint32_t linesize,
                std::vector<uint8_t>& output,
                uint32_t& out_linesize) override {
        if (!bgra_data || width == 0 || height == 0) {
            return false;
        }
        
        // 計算輸出大小
        size_t row_bytes = width * 4;  // BGRA = 4 bytes per pixel
        size_t total_size = row_bytes * height;
        
        output.resize(total_size);
        
        // 複製數據 (處理 linesize 可能與 row_bytes 不同的情況)
        if (linesize == row_bytes) {
            // 行寬相同，直接複製
            std::memcpy(output.data(), bgra_data, total_size);
        } else {
            // 逐行複製
            for (uint32_t y = 0; y < height; y++) {
                std::memcpy(output.data() + y * row_bytes,
                            bgra_data + y * linesize,
                            row_bytes);
            }
        }
        
        out_linesize = static_cast<uint32_t>(row_bytes);
        return true;
    }
};

// ========== Raw 解碼器 ==========
class RawDecoder : public IVideoDecoder {
public:
    VideoCodecType getCodecType() const override {
        return VideoCodecType::RAW_BGRA;
    }
    
    const char* getName() const override {
        return "Raw BGRA";
    }
    
    bool decode(const uint8_t* data, size_t size,
                uint32_t width, uint32_t height, uint32_t linesize,
                std::vector<uint8_t>& output) override {
        if (!data || size == 0 || width == 0 || height == 0) {
            return false;
        }
        
        size_t expected_size = linesize * height;
        if (size < expected_size) {
            blog(LOG_WARNING, "[RawDecoder] Data size %zu < expected %zu",
                 size, expected_size);
            return false;
        }
        
        // Raw 數據直接複製
        output.resize(size);
        std::memcpy(output.data(), data, size);
        
        return true;
    }
};

// ========== 工廠函數實現 (Raw 部分) ==========

std::unique_ptr<IVideoEncoder> createRawEncoder() {
    return std::make_unique<RawEncoder>();
}

std::unique_ptr<IVideoDecoder> createRawDecoder() {
    return std::make_unique<RawDecoder>();
}
