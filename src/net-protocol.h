/*
 * net-protocol.h
 * 定義客戶端與伺服器之間的通訊協議
 */

#ifndef NET_PROTOCOL_H
#define NET_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ========== 協議版本 ==========
#define PROTOCOL_VERSION 1

// ========== 預設端口 ==========
#define DEFAULT_SERVER_PORT 44555

// ========== 消息類型 (Client -> Server) ==========
typedef enum {
    // 請求屬性列表
    MSG_GET_PROPERTIES = 0x01,

    // 開始串流 (帶 JSON 設定)
    MSG_START_STREAM = 0x02,

    // 停止串流
    MSG_STOP_STREAM = 0x03,

    // 心跳包 (保持連接)
    MSG_HEARTBEAT = 0x04,

} ClientMessageType;

// ========== 消息類型 (Server -> Client) ==========
typedef enum {
    // 屬性列表回應 (JSON)
    MSG_PROPERTIES_RESPONSE = 0x81,

    // 視頻幀數據
    MSG_VIDEO_FRAME = 0x82,

    // 音頻幀數據
    MSG_AUDIO_FRAME = 0x83,

    // 錯誤消息
    MSG_ERROR = 0x8F,

} ServerMessageType;

// ========== 捕捉模式 ==========
typedef enum {
    CAPTURE_MODE_WINDOW = 0,
    CAPTURE_MODE_GAME = 1,
} CaptureMode;

// ========== 封包頭 ==========
#pragma pack(push, 1)

typedef struct {
    uint32_t magic;           // 魔數 'ORWC' (0x4F525743)
    uint8_t  version;         // 協議版本
    uint8_t  type;            // 消息類型
    uint16_t reserved;        // 保留
    uint32_t payload_size;    // 有效載荷大小
    uint32_t timestamp_ms;    // 時間戳 (毫秒)
} PacketHeader;

#define PACKET_MAGIC 0x4F525743  // 'ORWC'

// ========== 請求屬性封包 ==========
typedef struct {
    uint8_t capture_mode;     // CaptureMode
} GetPropertiesRequest;

// ========== 視頻幀封包 ==========
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t format;          // JPEG = 1
    uint32_t frame_number;
    // 後面跟隨 JPEG 數據
} VideoFrameHeader;

// ========== 音頻幀封包 ==========
typedef struct {
    uint32_t sample_rate;     // 如 48000
    uint32_t channels;        // 如 2 (立體聲)
    uint32_t format;          // 0 = Float32
    uint32_t samples;         // 樣本數
    // 後面跟隨 PCM 數據
} AudioFrameHeader;

#pragma pack(pop)

// ========== 輔助函數 ==========

static inline void packet_header_init(PacketHeader* header, uint8_t type, uint32_t payload_size) {
    header->magic = PACKET_MAGIC;
    header->version = PROTOCOL_VERSION;
    header->type = type;
    header->reserved = 0;
    header->payload_size = payload_size;
    header->timestamp_ms = 0; // 由發送者填充
}

static inline int packet_header_validate(const PacketHeader* header) {
    return (header->magic == PACKET_MAGIC && header->version == PROTOCOL_VERSION);
}

#ifdef __cplusplus
}
#endif

#endif // NET_PROTOCOL_H
