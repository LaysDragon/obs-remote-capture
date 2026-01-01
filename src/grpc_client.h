/*
 * grpc_client.h
 * gRPC 客戶端 C API 聲明
 */

#ifndef GRPC_CLIENT_H
#define GRPC_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 客戶端句柄
typedef struct grpc_client_handle* grpc_client_t;

// 創建/銷毀客戶端
grpc_client_t grpc_client_create(const char* server_address);
void grpc_client_destroy(grpc_client_t client);

// 連接狀態
bool grpc_client_is_connected(grpc_client_t client);
bool grpc_client_wait_connected(grpc_client_t client, int timeout_ms);

// 獲取屬性
bool grpc_client_get_properties(grpc_client_t client, const char* source_type);
size_t grpc_client_property_count(grpc_client_t client);
const char* grpc_client_property_name(grpc_client_t client, size_t index);
const char* grpc_client_property_description(grpc_client_t client, size_t index);
int grpc_client_property_type(grpc_client_t client, size_t index);
size_t grpc_client_property_item_count(grpc_client_t client, size_t index);
const char* grpc_client_property_item_name(grpc_client_t client, size_t prop_index, size_t item_index);
const char* grpc_client_property_item_value(grpc_client_t client, size_t prop_index, size_t item_index);

// 串流回調類型
typedef void (*grpc_video_callback_t)(uint32_t width, uint32_t height,
                                       const uint8_t* jpeg_data, size_t jpeg_size,
                                       uint64_t timestamp_ns, void* user_data);
typedef void (*grpc_audio_callback_t)(uint32_t sample_rate, uint32_t channels,
                                       const float* pcm_data, size_t samples,
                                       uint64_t timestamp_ns, void* user_data);

// 串流控制
bool grpc_client_start_stream(grpc_client_t client, const char* source_type,
                               const char** setting_keys, const char** setting_values, 
                               size_t settings_count,
                               grpc_video_callback_t on_video,
                               grpc_audio_callback_t on_audio,
                               void* user_data);
void grpc_client_stop_stream(grpc_client_t client);
bool grpc_client_is_streaming(grpc_client_t client);

#ifdef __cplusplus
}
#endif

#endif  // GRPC_CLIENT_H
