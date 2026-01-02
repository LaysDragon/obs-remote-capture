/*
 * codec_jpeg.cpp
 * JPEG 編解碼器實現 (使用 TurboJPEG)
 */

#include "video_codec.h"
#include <obs-module.h>

#ifdef HAVE_TURBOJPEG
#include <turbojpeg.h>
#endif

// ========== JPEG 編碼器 ==========
class JpegEncoder : public IVideoEncoder {
public:
    JpegEncoder() {
#ifdef HAVE_TURBOJPEG
        compressor_ = tjInitCompress();
        if (!compressor_) {
            blog(LOG_ERROR, "[JpegEncoder] Failed to initialize TurboJPEG compressor");
        }
#endif
    }
    
    ~JpegEncoder() override {
#ifdef HAVE_TURBOJPEG
        if (compressor_) {
            tjDestroy(compressor_);
        }
#endif
    }
    
    VideoCodecType getCodecType() const override {
        return VideoCodecType::JPEG;
    }
    
    const char* getName() const override {
        return "JPEG (TurboJPEG)";
    }
    
    bool encode(const uint8_t* bgra_data, 
                uint32_t width, uint32_t height, uint32_t linesize,
                std::vector<uint8_t>& output,
                uint32_t& out_linesize) override {
#ifdef HAVE_TURBOJPEG
        if (!compressor_ || !bgra_data || width == 0 || height == 0) {
            return false;
        }
        
        unsigned long jpeg_size = 0;
        unsigned char* jpeg_buf = nullptr;
        
        int result = tjCompress2(
            compressor_,
            bgra_data, width, linesize, height,
            TJPF_BGRA,
            &jpeg_buf, &jpeg_size,
            TJSAMP_420, quality_, TJFLAG_FASTDCT
        );
        
        if (result != 0 || !jpeg_buf) {
            blog(LOG_WARNING, "[JpegEncoder] tjCompress2 failed: %s",
                 tjGetErrorStr2(compressor_));
            if (jpeg_buf) tjFree(jpeg_buf);
            return false;
        }
        
        output.resize(jpeg_size);
        std::memcpy(output.data(), jpeg_buf, jpeg_size);
        tjFree(jpeg_buf);
        
        out_linesize = 0;  // JPEG 不使用 linesize
        return true;
#else
        (void)bgra_data;
        (void)width;
        (void)height;
        (void)linesize;
        (void)output;
        (void)out_linesize;
        blog(LOG_ERROR, "[JpegEncoder] TurboJPEG not available");
        return false;
#endif
    }
    
    void setQuality(int quality) {
        quality_ = quality;
    }

private:
#ifdef HAVE_TURBOJPEG
    tjhandle compressor_ = nullptr;
#endif
    int quality_ = 85;
};

// ========== JPEG 解碼器 ==========
class JpegDecoder : public IVideoDecoder {
public:
    JpegDecoder() {
#ifdef HAVE_TURBOJPEG
        decompressor_ = tjInitDecompress();
        if (!decompressor_) {
            blog(LOG_ERROR, "[JpegDecoder] Failed to initialize TurboJPEG decompressor");
        }
#endif
    }
    
    ~JpegDecoder() override {
#ifdef HAVE_TURBOJPEG
        if (decompressor_) {
            tjDestroy(decompressor_);
        }
#endif
    }
    
    VideoCodecType getCodecType() const override {
        return VideoCodecType::JPEG;
    }
    
    const char* getName() const override {
        return "JPEG (TurboJPEG)";
    }
    
    bool decode(const uint8_t* data, size_t size,
                uint32_t width, uint32_t height, uint32_t linesize,
                std::vector<uint8_t>& output) override {
        (void)linesize;  // JPEG 不使用輸入的 linesize
        
#ifdef HAVE_TURBOJPEG
        if (!decompressor_ || !data || size == 0) {
            return false;
        }
        
        // 先讀取 JPEG header 獲取實際尺寸
        int w, h, subsamp, colorspace;
        if (tjDecompressHeader3(decompressor_, data, static_cast<unsigned long>(size),
                                &w, &h, &subsamp, &colorspace) != 0) {
            blog(LOG_WARNING, "[JpegDecoder] tjDecompressHeader3 failed: %s",
                 tjGetErrorStr2(decompressor_));
            return false;
        }
        
        // 分配輸出緩衝區
        size_t buffer_size = w * h * 4;  // BGRA
        output.resize(buffer_size);
        
        // 解碼
        if (tjDecompress2(decompressor_, data, static_cast<unsigned long>(size),
                          output.data(), w, 0, h,
                          TJPF_BGRA, TJFLAG_FASTDCT) != 0) {
            blog(LOG_WARNING, "[JpegDecoder] tjDecompress2 failed: %s",
                 tjGetErrorStr2(decompressor_));
            return false;
        }
        
        return true;
#else
        (void)data;
        (void)size;
        (void)width;
        (void)height;
        (void)output;
        blog(LOG_ERROR, "[JpegDecoder] TurboJPEG not available");
        return false;
#endif
    }

private:
#ifdef HAVE_TURBOJPEG
    tjhandle decompressor_ = nullptr;
#endif
};

// ========== 工廠函數實現 (JPEG 部分) ==========

bool isJpegAvailable() {
#ifdef HAVE_TURBOJPEG
    return true;
#else
    return false;
#endif
}

std::unique_ptr<IVideoEncoder> createJpegEncoder() {
#ifdef HAVE_TURBOJPEG
    return std::make_unique<JpegEncoder>();
#else
    return nullptr;
#endif
}

std::unique_ptr<IVideoDecoder> createJpegDecoder() {
#ifdef HAVE_TURBOJPEG
    return std::make_unique<JpegDecoder>();
#else
    return nullptr;
#endif
}
