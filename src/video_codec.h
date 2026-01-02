/*
 * video_codec.h
 * 視頻編解碼器抽象介面
 */

#ifndef VIDEO_CODEC_H
#define VIDEO_CODEC_H

#include <cstdint>
#include <vector>
#include <memory>
#include <string>

// 編碼類型 (與 proto 中的 VideoCodec enum 對應)
enum class VideoCodecType {
    RAW_BGRA = 0,
    JPEG = 1,
    H264 = 2
};

// ========== 編碼器介面 ==========
class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;
    
    // 返回此編碼器的類型
    virtual VideoCodecType getCodecType() const = 0;
    
    // 返回編碼器名稱 (用於日誌)
    virtual const char* getName() const = 0;
    
    // 編碼一幀
    // 輸入: BGRA 格式的原始像素數據
    // 輸出: 編碼後的數據寫入 output
    // 返回: 成功返回 true
    virtual bool encode(const uint8_t* bgra_data, 
                        uint32_t width, uint32_t height, uint32_t linesize,
                        std::vector<uint8_t>& output,
                        uint32_t& out_linesize) = 0;
};

// ========== 解碼器介面 ==========
class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;
    
    // 返回此解碼器支援的編碼類型
    virtual VideoCodecType getCodecType() const = 0;
    
    // 返回解碼器名稱 (用於日誌)
    virtual const char* getName() const = 0;
    
    // 解碼一幀
    // 輸入: 編碼後的數據, 尺寸資訊
    // 輸出: BGRA 格式的像素數據寫入 output
    // 返回: 成功返回 true
    virtual bool decode(const uint8_t* data, size_t size,
                        uint32_t width, uint32_t height, uint32_t linesize,
                        std::vector<uint8_t>& output) = 0;
};

// ========== 工廠函數 ==========

// 創建編碼器 (根據可用性自動選擇最佳編碼器)
std::unique_ptr<IVideoEncoder> createDefaultEncoder();

// 創建指定類型的編碼器 (如果不支援則返回 nullptr)
std::unique_ptr<IVideoEncoder> createEncoder(VideoCodecType type);

// 創建解碼器 (根據編碼類型選擇)
std::unique_ptr<IVideoDecoder> createDecoder(VideoCodecType type);

// 檢查某種編碼器是否可用
bool isEncoderAvailable(VideoCodecType type);

#endif // VIDEO_CODEC_H
