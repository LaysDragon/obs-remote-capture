/*
 * video_codec.cpp
 * 編解碼器工廠函數實現
 */

#include "video_codec.h"
#include <obs-module.h>

// Forward declarations from codec implementations
std::unique_ptr<IVideoEncoder> createRawEncoder();
std::unique_ptr<IVideoDecoder> createRawDecoder();
std::unique_ptr<IVideoEncoder> createJpegEncoder();
std::unique_ptr<IVideoDecoder> createJpegDecoder();
bool isJpegAvailable();

// ========== 工廠函數實現 ==========

bool isEncoderAvailable(VideoCodecType type) {
    switch (type) {
        case VideoCodecType::RAW_BGRA:
            return true;  // 永遠可用
        case VideoCodecType::JPEG:
            return isJpegAvailable();
        case VideoCodecType::H264:
            return false;  // 未實現
        default:
            return false;
    }
}

std::unique_ptr<IVideoEncoder> createEncoder(VideoCodecType type) {
    switch (type) {
        case VideoCodecType::RAW_BGRA:
            return createRawEncoder();
        case VideoCodecType::JPEG:
            return createJpegEncoder();
        case VideoCodecType::H264:
            blog(LOG_WARNING, "[Codec] H.264 encoder not implemented");
            return nullptr;
        default:
            return nullptr;
    }
}

std::unique_ptr<IVideoEncoder> createDefaultEncoder() {
    // 優先使用 JPEG (如果可用)，否則使用 Raw
    if (isJpegAvailable()) {
        blog(LOG_INFO, "[Codec] Using JPEG encoder (TurboJPEG available)");
        return createJpegEncoder();
    }
    
    blog(LOG_INFO, "[Codec] Using Raw BGRA encoder (no compression)");
    return createRawEncoder();
}

std::unique_ptr<IVideoDecoder> createDecoder(VideoCodecType type) {
    switch (type) {
        case VideoCodecType::RAW_BGRA:
            return createRawDecoder();
        case VideoCodecType::JPEG:
            return createJpegDecoder();
        case VideoCodecType::H264:
            blog(LOG_WARNING, "[Codec] H.264 decoder not implemented");
            return nullptr;
        default:
            return nullptr;
    }
}
