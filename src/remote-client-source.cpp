/*
 * remote-client-source.cpp
 * 接收端來源 - Session-based API
 *
 * 功能:
 * 1. 連接 gRPC 服務器並創建 Session
 * 2. 獲取可用 source 列表，選擇 source type
 * 3. 動態接收屬性更新，完全被動
 * 4. 串流綁定 session_id
 */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/threading.h>
#include <util/platform.h>

#include "grpc_client.h"

// 禁用 protobuf 生成代碼的警告
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4267)
#endif

#include "remote_capture.pb.h"

#ifdef _MSC_VER
#pragma warning(pop)
#endif

using namespace obsremote;

#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <vector>
#include <string>
#include <cstring>
#include <sstream>
#include <map>

// H.264 解碼器
#include "codec_ffmpeg.h"

#define DEFAULT_SERVER_PORT 44555

// ========== 來源數據結構 ==========
struct remote_source_data {
    obs_source_t* source;

    // 連接設定
    std::string server_ip;
    int server_port;

    // gRPC 客戶端
    std::unique_ptr<GrpcClient> grpc_client;
    std::atomic<bool> connected;
    std::atomic<bool> streaming;
    
    // Session
    std::string session_id;
    std::string current_source_type;
    
    // 可用 source 列表 (從 server 獲取)
    std::vector<GrpcClient::SourceInfo> available_sources;
    
    // 當前屬性緩存 (完全動態，從 server 獲取)
    std::mutex props_mutex;
    std::vector<GrpcClient::Property> cached_props;

    // 視頻幀緩衝
    std::mutex video_mutex;
    gs_texture_t* texture;
    uint32_t tex_width;
    uint32_t tex_height;
    std::vector<uint8_t> frame_buffer;
    bool frame_ready;

    // 音頻
    bool audio_capture;

    // H.264 解碼器
    std::unique_ptr<FFmpegDecoder> h264_decoder;

    remote_source_data() :
        source(nullptr),
        server_port(DEFAULT_SERVER_PORT),
        connected(false),
        streaming(false),
        texture(nullptr),
        tex_width(0),
        tex_height(0),
        frame_ready(false),
        audio_capture(false)
    {}
};

// ========== 連接伺服器並創建 Session ==========
static bool connect_and_create_session(remote_source_data* data) {
    if (data->connected.load()) return true;
    if (data->server_ip.empty()) return false;

    std::string address = data->server_ip + ":" + std::to_string(data->server_port);
    data->grpc_client = std::make_unique<GrpcClient>(address);
    
    if (!data->grpc_client->waitForConnected(5000)) {
        blog(LOG_WARNING, "[Remote Source] Failed to connect to %s", address.c_str());
        data->grpc_client.reset();
        return false;
    }
    
    blog(LOG_INFO, "[Remote Source] Connected to %s", address.c_str());
    
    // 獲取可用 source 列表
    if (!data->grpc_client->getAvailableSources(data->available_sources)) {
        blog(LOG_WARNING, "[Remote Source] Failed to get available sources");
    } else {
        blog(LOG_INFO, "[Remote Source] Got %zu available sources", data->available_sources.size());
    }
    
    // 創建 session
    if (!data->grpc_client->createSession(data->session_id)) {
        blog(LOG_WARNING, "[Remote Source] Failed to create session");
        data->grpc_client.reset();
        return false;
    }
    
    blog(LOG_INFO, "[Remote Source] Created session: %s", data->session_id.c_str());
    data->connected.store(true);
    return true;
}

static void disconnect_and_release_session(remote_source_data* data) {
    data->streaming.store(false);
    
    if (data->grpc_client) {
        data->grpc_client->stopStream();
        
        if (!data->session_id.empty()) {
            data->grpc_client->releaseSession(data->session_id);
            data->session_id.clear();
        }
        
        data->grpc_client.reset();
    }
    
    data->connected.store(false);
    data->current_source_type.clear();
}

// ========== gRPC 回調函數 ==========
static void grpc_video_callback(uint32_t width, uint32_t height,
                                 int codec,
                                 const uint8_t* frame_data, size_t frame_size,
                                 uint32_t linesize,
                                 uint64_t timestamp_ns, void* user_data) {
    UNUSED_PARAMETER(linesize);
    remote_source_data* data = (remote_source_data*)user_data;
    if (!data) return;
    
    static uint32_t callback_count = 0;
    callback_count++;
    
    if (codec != VideoCodec::CODEC_H264) {
        if (callback_count % 100 == 1) {
            blog(LOG_WARNING, "[Remote Source] Unsupported codec: %d", codec);
        }
        return;
    }
    
    // 確保解碼器存在
    if (!data->h264_decoder) {
        data->h264_decoder = std::make_unique<FFmpegDecoder>();
        if (!data->h264_decoder->init()) {
            blog(LOG_ERROR, "[Remote Source] Failed to create H.264 decoder");
            data->h264_decoder.reset();
            return;
        }
        blog(LOG_INFO, "[Remote Source] Created H.264 decoder: %s", 
             data->h264_decoder->getName());
    }
    
    // 解碼
    std::vector<uint8_t> decoded;
    uint32_t decoded_width = 0, decoded_height = 0;
    if (data->h264_decoder->decode(frame_data, frame_size, width, height, 
                                     decoded, decoded_width, decoded_height)) {
        std::lock_guard<std::mutex> lock(data->video_mutex);
        data->tex_width = decoded_width;
        data->tex_height = decoded_height;
        data->frame_buffer = std::move(decoded);
        data->frame_ready = true;
        
        if (callback_count % 100 == 1) {
            uint64_t latency_ms = (os_gettime_ns() - timestamp_ns) / 1000000;
            blog(LOG_INFO, "[Remote Source] Frame #%u: %ux%u, latency=%llu ms",
                 callback_count, decoded_width, decoded_height, (unsigned long long)latency_ms);
        }
    }
}

static void grpc_audio_callback(uint32_t sample_rate, uint32_t channels,
                                 const float* pcm_data, size_t samples,
                                 uint64_t timestamp_ns, void* user_data) {
    remote_source_data* data = (remote_source_data*)user_data;
    if (!data || !data->audio_capture) return;
    
    struct obs_source_audio audio = {};
    audio.data[0] = (uint8_t*)pcm_data;
    audio.frames = (uint32_t)samples;
    audio.speakers = (channels == 2) ? SPEAKERS_STEREO : SPEAKERS_MONO;
    audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
    audio.samples_per_sec = sample_rate;
    audio.timestamp = timestamp_ns;
    
    obs_source_output_audio(data->source, &audio);
}

// ========== 開始串流 ==========
static void start_streaming(remote_source_data* data) {
    if (data->streaming.load()) return;
    if (data->session_id.empty()) return;
    
    blog(LOG_INFO, "[Remote Source] Starting stream for session %s", data->session_id.c_str());
    
    data->streaming.store(true);
    
    remote_source_data* capture_data = data;
    
    if (!data->grpc_client->startStream(data->session_id,
            [capture_data](uint32_t w, uint32_t h, int codec, 
                           const uint8_t* d, size_t s, uint32_t ls, uint64_t t) {
                grpc_video_callback(w, h, codec, d, s, ls, t, capture_data);
            },
            [capture_data](uint32_t sr, uint32_t ch, const float* d, size_t s, uint64_t t) {
                grpc_audio_callback(sr, ch, d, s, t, capture_data);
            })) {
        blog(LOG_WARNING, "[Remote Source] Failed to start stream");
        data->streaming.store(false);
    } else {
        blog(LOG_INFO, "[Remote Source] Stream started");
    }
}

static void stop_streaming(remote_source_data* data) {
    if (!data->streaming.load()) return;
    
    data->streaming.store(false);
    
    if (data->grpc_client) {
        data->grpc_client->stopStream();
    }
    
    blog(LOG_INFO, "[Remote Source] Stream stopped");
}

// ========== OBS 來源回調 ==========
static const char* remote_source_get_name(void* unused) {
    UNUSED_PARAMETER(unused);
    return "Remote Source (遠端來源)";
}

static void* remote_source_create(obs_data_t* settings, obs_source_t* source) {
    remote_source_data* data = new remote_source_data();
    data->source = source;

    data->server_ip = obs_data_get_string(settings, "server_ip");
    data->server_port = (int)obs_data_get_int(settings, "server_port");
    data->audio_capture = obs_data_get_bool(settings, "audio_capture");
    
    obs_source_set_audio_mixers(source, 0x3F);
    obs_source_set_audio_active(source, true);

    return data;
}

static void remote_source_destroy(void* data_ptr) {
    remote_source_data* data = (remote_source_data*)data_ptr;

    stop_streaming(data);
    disconnect_and_release_session(data);

    obs_enter_graphics();
    if (data->texture) {
        gs_texture_destroy(data->texture);
    }
    obs_leave_graphics();

    data->h264_decoder.reset();

    delete data;
}

static void remote_source_update(void* data_ptr, obs_data_t* settings) {
    remote_source_data* data = (remote_source_data*)data_ptr;

    std::string new_ip = obs_data_get_string(settings, "server_ip");
    int new_port = (int)obs_data_get_int(settings, "server_port");
    bool new_audio = obs_data_get_bool(settings, "audio_capture");
    std::string new_source_type = obs_data_get_string(settings, "source_type");

    bool reconnect = (new_ip != data->server_ip || new_port != data->server_port);
    
    data->audio_capture = new_audio;

    if (reconnect) {
        stop_streaming(data);
        disconnect_and_release_session(data);
        data->server_ip = new_ip;
        data->server_port = new_port;
    }
    
    // 連接並創建 session (如果需要)
    if (!data->connected.load() && !data->server_ip.empty()) {
        connect_and_create_session(data);
    }
    
    // 切換 source type (如果已連接且類型不同)
    if (data->connected.load() && !new_source_type.empty() && 
        new_source_type != data->current_source_type) {
        
        blog(LOG_INFO, "[Remote Source] Switching source type to %s", new_source_type.c_str());
        
        std::vector<GrpcClient::Property> new_props;
        if (data->grpc_client->setSourceType(data->session_id, new_source_type, new_props)) {
            data->current_source_type = new_source_type;
            
            std::lock_guard<std::mutex> lock(data->props_mutex);
            data->cached_props = std::move(new_props);
            
            blog(LOG_INFO, "[Remote Source] Source type set, got %zu properties", 
                 data->cached_props.size());
        }
    }
    
    // 收集並應用 child_ 開頭的設定
    if (data->connected.load() && !data->current_source_type.empty()) {
        std::map<std::string, std::string> child_settings;
        obs_data_item_t* item = obs_data_first(settings);
        const char* prefix = "child_";
        const size_t prefix_len = 6;
        
        while (item) {
            const char* key = obs_data_item_get_name(item);
            if (strncmp(key, prefix, prefix_len) == 0) {
                const char* real_key = key + prefix_len;
                //TODO: only string??
                if (obs_data_item_gettype(item) == OBS_DATA_STRING) {
                    const char* val = obs_data_item_get_string(item);
                    if (val) child_settings[real_key] = val;
                }
            }
            obs_data_item_next(&item);
        }
        
        if (!child_settings.empty()) {
            std::vector<GrpcClient::Property> new_props;
            if (data->grpc_client->updateSettings(data->session_id, child_settings, new_props)) {
                std::lock_guard<std::mutex> lock(data->props_mutex);
                data->cached_props = std::move(new_props);
                blog(LOG_INFO, "[Remote Source] Settings updated, refreshed %zu properties",
                     data->cached_props.size());
            }
        }
    }
    
    // 開始串流 (如果已設定 source type)
    if (data->connected.load() && !data->current_source_type.empty() && !data->streaming.load()) {
        start_streaming(data);
    }
}

static uint32_t remote_source_get_width(void* data_ptr) {
    remote_source_data* data = (remote_source_data*)data_ptr;
    return data->tex_width;
}

static uint32_t remote_source_get_height(void* data_ptr) {
    remote_source_data* data = (remote_source_data*)data_ptr;
    return data->tex_height;
}

static void remote_source_video_render(void* data_ptr, gs_effect_t* effect) {
    remote_source_data* data = (remote_source_data*)data_ptr;

    {
        std::lock_guard<std::mutex> lock(data->video_mutex);
        if (data->frame_ready && !data->frame_buffer.empty()) {
            size_t expected_size = (size_t)data->tex_width * data->tex_height * 4;
            if (data->frame_buffer.size() != expected_size) {
                static uint32_t mismatch_count = 0;
                if (++mismatch_count % 30 == 1) {
                    blog(LOG_WARNING, "[Remote Source] Frame size mismatch");
                }
                data->frame_ready = false;
                return;
            }
            
            if (!data->texture ||
                data->tex_width != gs_texture_get_width(data->texture) ||
                data->tex_height != gs_texture_get_height(data->texture)) {

                if (data->texture) {
                    gs_texture_destroy(data->texture);
                }
                data->texture = gs_texture_create(
                    data->tex_width, data->tex_height,
                    GS_BGRA, 1, nullptr, GS_DYNAMIC);
            }

            if (data->texture) {
                gs_texture_set_image(data->texture,
                    data->frame_buffer.data(),
                    data->tex_width * 4, false);
            }
            data->frame_ready = false;
        }
    }

    if (data->texture) {
        effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
        gs_technique_t* tech = gs_effect_get_technique(effect, "Draw");
        gs_technique_begin(tech);
        gs_technique_begin_pass(tech, 0);

        gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"),
                              data->texture);
        gs_draw_sprite(data->texture, 0, data->tex_width, data->tex_height);

        gs_technique_end_pass(tech);
        gs_technique_end(tech);
    }
}

static void remote_source_video_tick(void* data_ptr, float seconds) {
    UNUSED_PARAMETER(seconds);
    UNUSED_PARAMETER(data_ptr);
}

// ========== 屬性面板 ==========
//TODO: where is reconnect/reload from server action? 
static bool on_refresh_clicked(obs_properties_t* props, obs_property_t* p,
                               void* data_ptr) {
    UNUSED_PARAMETER(p);
    remote_source_data* data = (remote_source_data*)data_ptr;
    
    if (!data->connected.load()) {
        connect_and_create_session(data);
    }
    
    if (!data->connected.load()) return false;
    
    // 更新 source type 列表
    obs_property_t* source_list = obs_properties_get(props, "source_type");
    obs_property_list_clear(source_list);
    
    for (const auto& src : data->available_sources) {
        obs_property_list_add_string(source_list, src.display_name.c_str(), src.id.c_str());
    }
    
    return true;
}

static bool on_source_type_changed(obs_properties_t* props, obs_property_t* p,
                                    obs_data_t* settings) {
    UNUSED_PARAMETER(p);
    
    remote_source_data* data = (remote_source_data*)obs_properties_get_param(props);
    if (!data || !data->connected.load()) return false;
    
    const char* new_type = obs_data_get_string(settings, "source_type");
    if (!new_type || strlen(new_type) == 0) return false;
    if (data->current_source_type == new_type) return false;
    
    blog(LOG_INFO, "[Remote Source] Source type changed to %s", new_type);
    
    // 設定新的 source type
    std::vector<GrpcClient::Property> new_props;
    if (!data->grpc_client->setSourceType(data->session_id, new_type, new_props)) {
        return false;
    }
    
    data->current_source_type = new_type;
    
    {
        std::lock_guard<std::mutex> lock(data->props_mutex);
        data->cached_props = std::move(new_props);
    }
    
    // 移除舊的 child_ 屬性
    std::vector<std::string> to_remove;
    obs_property_t* prop = obs_properties_first(props);
    while (prop) {
        const char* name = obs_property_name(prop);
        if (strncmp(name, "child_", 6) == 0) {
            to_remove.push_back(name);
        }
        obs_property_next(&prop);
    }
    for (const auto& name : to_remove) {
        obs_properties_remove_by_name(props, name.c_str());
    }
    
    // 添加新的 child_ 屬性
    {
        std::lock_guard<std::mutex> lock(data->props_mutex);
        for (const auto& cprop : data->cached_props) {
            std::string child_name = "child_" + cprop.name;
            
            switch (cprop.type) {
            case OBS_PROPERTY_LIST: {
                obs_property_t* list = obs_properties_add_list(props,
                    child_name.c_str(), cprop.description.c_str(),
                    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
                for (const auto& item : cprop.items) {
                    obs_property_list_add_string(list, item.name.c_str(), item.value.c_str());
                }
                break;
            }
            case OBS_PROPERTY_BOOL:
                obs_properties_add_bool(props, child_name.c_str(), cprop.description.c_str());
                break;
            case OBS_PROPERTY_INT:
                obs_properties_add_int(props, child_name.c_str(), cprop.description.c_str(),
                    cprop.min_int, cprop.max_int, cprop.step_int);
                break;
            case OBS_PROPERTY_FLOAT:
                obs_properties_add_float(props, child_name.c_str(), cprop.description.c_str(),
                    cprop.min_float, cprop.max_float, cprop.step_float);
                break;
            case OBS_PROPERTY_TEXT:
                obs_properties_add_text(props, child_name.c_str(), cprop.description.c_str(),
                    OBS_TEXT_DEFAULT);
                break;
            default:
                break;
            }
        }
    }
    
    return true;
}

static obs_properties_t* remote_source_properties(void* data_ptr) {
    remote_source_data* data = (remote_source_data*)data_ptr;

    obs_properties_t* props = obs_properties_create();
    obs_properties_set_param(props, data, nullptr);

    // 伺服器設定
    obs_properties_add_text(props, "server_ip",
        "伺服器 IP (Server IP)", OBS_TEXT_DEFAULT);
    obs_properties_add_int(props, "server_port",
        "端口 (Port)", 1, 65535, 1);

    // 刷新按鈕
    obs_properties_add_button(props, "refresh",
        "連接/刷新 (Connect/Refresh)", on_refresh_clicked);

    // Source 類型選擇 (從 server 獲取)
    obs_property_t* source_type = obs_properties_add_list(props, "source_type",
        "來源類型 (Source Type)", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_set_modified_callback(source_type, on_source_type_changed);
    
    // 填充已有的 source 列表
    if (data) {
        for (const auto& src : data->available_sources) {
            obs_property_list_add_string(source_type, src.display_name.c_str(), src.id.c_str());
        }
    }
    
    // 動態添加 child_ 屬性 (如果已有緩存)
    if (data) {
        std::lock_guard<std::mutex> lock(data->props_mutex);
        for (const auto& cprop : data->cached_props) {
            std::string child_name = "child_" + cprop.name;
            
            switch (cprop.type) {
            case OBS_PROPERTY_LIST: {
                obs_property_t* list = obs_properties_add_list(props,
                    child_name.c_str(), cprop.description.c_str(),
                    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
                for (const auto& item : cprop.items) {
                    obs_property_list_add_string(list, item.name.c_str(), item.value.c_str());
                }
                break;
            }
            case OBS_PROPERTY_BOOL:
                obs_properties_add_bool(props, child_name.c_str(), cprop.description.c_str());
                break;
            case OBS_PROPERTY_INT:
                obs_properties_add_int(props, child_name.c_str(), cprop.description.c_str(),
                    cprop.min_int, cprop.max_int, cprop.step_int);
                break;
            case OBS_PROPERTY_FLOAT:
                obs_properties_add_float(props, child_name.c_str(), cprop.description.c_str(),
                    cprop.min_float, cprop.max_float, cprop.step_float);
                break;
            case OBS_PROPERTY_TEXT:
                obs_properties_add_text(props, child_name.c_str(), cprop.description.c_str(),
                    OBS_TEXT_DEFAULT);
                break;
            default:
                break;
            }
        }
    }

    // 音頻捕捉
    obs_properties_add_bool(props, "audio_capture",
        "音效捕捉 (Audio Capture) [實驗性]");

    return props;
}

static void remote_source_get_defaults(obs_data_t* settings) {
    obs_data_set_default_string(settings, "server_ip", "");
    obs_data_set_default_int(settings, "server_port", DEFAULT_SERVER_PORT);
    obs_data_set_default_string(settings, "source_type", "");
    obs_data_set_default_bool(settings, "audio_capture", false);
}

// ========== 來源信息結構 ==========
extern "C" {

struct obs_source_info remote_source_info;

void init_remote_source_info() {
    memset(&remote_source_info, 0, sizeof(remote_source_info));
    remote_source_info.id = "remote_source";
    remote_source_info.type = OBS_SOURCE_TYPE_INPUT;
    remote_source_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO |
                                      OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
    remote_source_info.get_name = remote_source_get_name;
    remote_source_info.create = remote_source_create;
    remote_source_info.destroy = remote_source_destroy;
    remote_source_info.update = remote_source_update;
    remote_source_info.get_width = remote_source_get_width;
    remote_source_info.get_height = remote_source_get_height;
    remote_source_info.video_render = remote_source_video_render;
    remote_source_info.video_tick = remote_source_video_tick;
    remote_source_info.get_properties = remote_source_properties;
    remote_source_info.get_defaults = remote_source_get_defaults;
}

}  // extern "C"
