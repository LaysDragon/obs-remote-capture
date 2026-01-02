/*
 * remote-client-source.cpp
 * 接收端來源 - 作為 OBS 來源顯示遠端捕捉的視窗
 *
 * 功能:
 * 1. 通過 gRPC 連接到遠端伺服器
 * 2. 獲取視窗/遊戲列表並顯示在屬性面板
 * 3. 接收並渲染視頻/音頻數據
 */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/threading.h>
#include <util/platform.h>

#include "grpc_client.h"


// 禁用 protobuf 生成代碼的警告
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4267)  // size_t to int conversion
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

// 捕捉模式常數
#define CAPTURE_MODE_WINDOW 0
#define CAPTURE_MODE_GAME 1
#define DEFAULT_SERVER_PORT 44555

// ========== 來源數據結構 ==========
struct remote_source_data {
    obs_source_t* source;

    // 連接設定
    std::string server_ip;
    int server_port;
    int capture_mode;  // 0=Window, 1=Game

    // 當前選擇的視窗/遊戲
    std::string selected_window;

    // 音頻捕捉開關
    bool audio_capture;

    // gRPC 客戶端
    std::unique_ptr<GrpcClient> grpc_client;
    std::atomic<bool> connected;
    std::atomic<bool> streaming;

    // 視頻幀緩衝
    std::mutex video_mutex;
    gs_texture_t* texture;
    uint32_t tex_width;
    uint32_t tex_height;
    std::vector<uint8_t> frame_buffer;
    bool frame_ready;

    // 音頻緩衝
    std::mutex audio_mutex;
    std::queue<std::vector<float>> audio_queue;

    // 屬性列表緩存
    std::mutex props_mutex;
    std::vector<std::pair<std::string, std::string>> window_list;
    
    // 動態屬性緩存 (從遠端解析)
    struct CachedProperty {
        std::string name;
        std::string description;
        int type;  // OBS_PROPERTY_* 類型
        bool visible;
        
        // LIST 類型的選項
        std::vector<std::pair<std::string, std::string>> items;  // name, value
        int list_format;  // 0=string, 1=int
        std::string current_string;
        long long current_int;
        
        // BOOL 類型
        bool default_bool;
        
        // INT 類型
        int min_int, max_int, step_int, default_int;
        
        // FLOAT 類型
        double min_float, max_float, step_float, default_float;
        
        // TEXT 類型
        std::string default_text;
        
        CachedProperty() : type(0), visible(true), list_format(0), current_int(0),
                          default_bool(false), min_int(0), max_int(100), step_int(1), default_int(0),
                          min_float(0), max_float(1), step_float(0.01), default_float(0) {}
    };
    std::vector<CachedProperty> cached_props;

    // H.264 解碼器
    std::unique_ptr<FFmpegDecoder> h264_decoder;

    remote_source_data() :
        source(nullptr),
        server_port(DEFAULT_SERVER_PORT),
        capture_mode(CAPTURE_MODE_WINDOW),
        audio_capture(false),
        connected(false),
        streaming(false),
        texture(nullptr),
        tex_width(0),
        tex_height(0),
        frame_ready(false)
    {}
};

// ========== 連接伺服器 ==========
static bool connect_to_server(remote_source_data* data) {
    if (data->connected.load()) return true;
    if (data->server_ip.empty()) return false;

    // 創建 gRPC 客戶端並連接
    std::string address = data->server_ip + ":" + std::to_string(data->server_port);
    data->grpc_client = std::make_unique<GrpcClient>(address);
    
    if (data->grpc_client && data->grpc_client->waitForConnected(5000)) {
        data->connected.store(true);
        blog(LOG_INFO, "[Remote Source] gRPC connected to %s", address.c_str());
        return true;
    }
    
    blog(LOG_WARNING, "[Remote Source] gRPC failed to connect to %s", address.c_str());
    data->grpc_client.reset();
    return false;
}

static void disconnect_from_server(remote_source_data* data) {
    data->streaming.store(false);
    data->connected.store(false);

    if (data->grpc_client) {
        data->grpc_client->stopStream();
        data->grpc_client.reset();
    }
}

// ========== 請求屬性列表 ==========
// forward declaration
static void stop_streaming(remote_source_data* data);

static bool request_properties(remote_source_data* data) {
    // 如果正在串流，先停止並斷開連接以清除 socket 緩衝區
    if (data->streaming.load()) {
        blog(LOG_INFO, "[Remote Source] Stopping current stream before refreshing properties");
        stop_streaming(data);
    }
    
    // 斷開現有連接以確保乾淨的狀態
    if (data->connected.load()) {
        disconnect_from_server(data);
    }
    
    if (!connect_to_server(data)) return false;

    // 使用 gRPC 客戶端獲取屬性
    std::string source_type = (data->capture_mode == CAPTURE_MODE_GAME) 
        ? "game_capture" : "window_capture";
    
    if (!data->grpc_client->getProperties(source_type)) {
        blog(LOG_WARNING, "[Remote Source] gRPC GetProperties failed");
        return false;
    }
    
    // 從 gRPC 響應填充 cached_props
    {
        std::lock_guard<std::mutex> lock(data->props_mutex);
        data->cached_props.clear();
        data->window_list.clear();
        
        const auto& properties = data->grpc_client->getCachedProperties();
        for (const auto& grpc_prop : properties) {
            remote_source_data::CachedProperty prop;
            prop.name = grpc_prop.name;
            prop.description = grpc_prop.description;
            prop.type = grpc_prop.type;
            prop.visible = true;
            
            // 處理 LIST 類型
            if (prop.type == 6) {  // OBS_PROPERTY_LIST
                for (const auto& item : grpc_prop.items) {
                    prop.items.push_back({item.name, item.value});
                    
                    // 填充視窗列表 (如果是 window 屬性)
                    if (prop.name == "window") {
                        data->window_list.push_back({item.name, item.value});
                    }
                }
            }
            
            data->cached_props.push_back(prop);
        }
    }
    
    blog(LOG_INFO, "[Remote Source] gRPC: Got %zu properties, %zu windows",
         data->cached_props.size(), data->window_list.size());
    return true;
}

// ========== gRPC 回調函數 ==========
// gRPC 視頻回調
static void grpc_video_callback(uint32_t width, uint32_t height,
                                 int codec,
                                 const uint8_t* frame_data, size_t frame_size,
                                 uint32_t linesize,
                                 uint64_t timestamp_ns, void* user_data) {
    UNUSED_PARAMETER(linesize);
    remote_source_data* data = (remote_source_data*)user_data;
    if (!data) return;
    
    // 計數器用於日誌
    static uint32_t callback_count = 0;
    callback_count++;
    
    // 只處理 H.264
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
    if (data->h264_decoder->decode(frame_data, frame_size, width, height, decoded)) {
        std::lock_guard<std::mutex> lock(data->video_mutex);
        data->tex_width = width;
        data->tex_height = height;
        data->frame_buffer = std::move(decoded);
        data->frame_ready = true;
        
        // 計算端對端延遲
        uint64_t now = os_gettime_ns();
        uint64_t latency_ms = (now - timestamp_ns) / 1000000;
        
        // 每 100 幀輸出一次
        if (callback_count % 100 == 1) {
            blog(LOG_INFO, "[Remote Source] H.264 frame #%u: %ux%u, size=%zu, latency=%llu ms",
                 callback_count, width, height, frame_size, (unsigned long long)latency_ms);
        }
    } else {
        if (callback_count % 100 == 1) {
            blog(LOG_WARNING, "[Remote Source] H.264 decode failed for frame #%u", callback_count);
        }
    }
}

// gRPC 音頻回調
static void grpc_audio_callback(uint32_t sample_rate, uint32_t channels,
                                 const float* pcm_data, size_t samples,
                                 uint64_t timestamp_ns, void* user_data) {
    remote_source_data* data = (remote_source_data*)user_data;
    if (!data || !data->audio_capture) return;
    
    // 構建 OBS 音頻結構
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
    if (data->streaming.load()) {
        blog(LOG_DEBUG, "[Remote Source] Already streaming, ignoring start request");
        return;
    }
    if (!connect_to_server(data)) return;

    blog(LOG_INFO, "[Remote Source] Starting stream with window: %s", data->selected_window.c_str());

    // 使用 gRPC 客戶端開始串流
    std::string source_type = (data->capture_mode == CAPTURE_MODE_GAME) 
        ? "game_capture" : "window_capture";
    
    // 構建設定
    std::map<std::string, std::string> settings;
    settings["window"] = data->selected_window;
    
    data->streaming.store(true);
    
    // 使用 lambda 捕獲 data 指針來調用回調
    remote_source_data* capture_data = data;
    
    if (!data->grpc_client->startStream(source_type, settings,
            [capture_data](uint32_t w, uint32_t h, int codec, 
                           const uint8_t* d, size_t s, uint32_t ls, uint64_t t) {
                grpc_video_callback(w, h, codec, d, s, ls, t, capture_data);
            },
            [capture_data](uint32_t sr, uint32_t ch, const float* d, size_t s, uint64_t t) {
                grpc_audio_callback(sr, ch, d, s, t, capture_data);
            })) {
        blog(LOG_WARNING, "[Remote Source] gRPC StartStream failed");
        data->streaming.store(false);
    } else {
        blog(LOG_INFO, "[Remote Source] gRPC stream started");
    }
}

static void stop_streaming(remote_source_data* data) {
    if (!data->streaming.load()) return;

    blog(LOG_INFO, "[Remote Source] Stopping stream");

    // 先標記停止
    data->streaming.store(false);

    // 停止 gRPC 串流
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

    // 解碼器會在接收時動態創建

    // 載入設定
    data->server_ip = obs_data_get_string(settings, "server_ip");
    data->server_port = (int)obs_data_get_int(settings, "server_port");
    data->capture_mode = (int)obs_data_get_int(settings, "capture_mode");
    data->selected_window = obs_data_get_string(settings, "selected_window");
    data->audio_capture = obs_data_get_bool(settings, "audio_capture");
    
    // 設置音頻狀態
    obs_source_set_audio_mixers(source, 0x3F);  // 啟用所有音頻軌道
    obs_source_set_audio_active(source, true);  // 啟用音頻

    return data;
}

static void remote_source_destroy(void* data_ptr) {
    remote_source_data* data = (remote_source_data*)data_ptr;

    stop_streaming(data);
    disconnect_from_server(data);

    // 釋放紋理
    obs_enter_graphics();
    if (data->texture) {
        gs_texture_destroy(data->texture);
    }
    obs_leave_graphics();

    // 清理 H.264 解碼器
    data->h264_decoder.reset();

    delete data;
}

static void remote_source_update(void* data_ptr, obs_data_t* settings) {
    remote_source_data* data = (remote_source_data*)data_ptr;

    std::string new_ip = obs_data_get_string(settings, "server_ip");
    int new_port = (int)obs_data_get_int(settings, "server_port");
    int new_mode = (int)obs_data_get_int(settings, "capture_mode");
    std::string new_window = obs_data_get_string(settings, "selected_window");
    bool new_audio = obs_data_get_bool(settings, "audio_capture");

    bool reconnect = (new_ip != data->server_ip || new_port != data->server_port);
    bool restart_stream = (new_mode != data->capture_mode ||
                           new_window != data->selected_window ||
                           new_audio != data->audio_capture);

    if (reconnect) {
        stop_streaming(data);
        disconnect_from_server(data);
    } else if (restart_stream && data->streaming.load()) {
        stop_streaming(data);
    }

    data->server_ip = new_ip;
    data->server_port = new_port;
    data->capture_mode = new_mode;
    data->selected_window = new_window;
    data->audio_capture = new_audio;

    // 如果有選擇視窗，則開始串流
    if (!data->selected_window.empty()) {
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

    // 更新紋理
    {
        std::lock_guard<std::mutex> lock(data->video_mutex);
        if (data->frame_ready && !data->frame_buffer.empty()) {
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

    // 渲染
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
    remote_source_data* data = (remote_source_data*)data_ptr;

    // 處理音頻隊列
    std::lock_guard<std::mutex> lock(data->audio_mutex);
    while (!data->audio_queue.empty()) {
        auto& audio = data->audio_queue.front();

        struct obs_source_audio obs_audio;
        memset(&obs_audio, 0, sizeof(obs_audio));
        obs_audio.data[0] = (uint8_t*)audio.data();
        obs_audio.frames = (uint32_t)(audio.size() / 2);  // 假設立體聲
        obs_audio.speakers = SPEAKERS_STEREO;
        obs_audio.format = AUDIO_FORMAT_FLOAT;
        obs_audio.samples_per_sec = 48000;
        obs_audio.timestamp = os_gettime_ns();

        obs_source_output_audio(data->source, &obs_audio);
        data->audio_queue.pop();
    }
}

// ========== 屬性面板 ==========
static bool on_refresh_clicked(obs_properties_t* props, obs_property_t* p,
                               void* data_ptr) {
    UNUSED_PARAMETER(p);
    remote_source_data* data = (remote_source_data*)data_ptr;

    if (!request_properties(data)) {
        return false;
    }

    // 更新視窗列表
    obs_property_t* window_list = obs_properties_get(props, "selected_window");
    obs_property_list_clear(window_list);

    std::lock_guard<std::mutex> lock(data->props_mutex);
    for (const auto& item : data->window_list) {
        obs_property_list_add_string(window_list,
            item.first.c_str(), item.second.c_str());
    }

    return true;
}

static obs_properties_t* remote_source_properties(void* data_ptr) {
    remote_source_data* data = (remote_source_data*)data_ptr;

    obs_properties_t* props = obs_properties_create();

    // 伺服器設定
    obs_properties_add_text(props, "server_ip",
        "伺服器 IP (Server IP)", OBS_TEXT_DEFAULT);
    obs_properties_add_int(props, "server_port",
        "端口 (Port)", 1, 65535, 1);

    // 捕捉模式
    obs_property_t* mode = obs_properties_add_list(props, "capture_mode",
        "捕捉模式 (Mode)", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(mode, "視窗捕捉 (Window)", CAPTURE_MODE_WINDOW);
    obs_property_list_add_int(mode, "遊戲捕捉 (Game)", CAPTURE_MODE_GAME);

    // 刷新按鈕
    obs_properties_add_button(props, "refresh",
        "刷新列表 (Refresh)", on_refresh_clicked);

    // 動態添加遠端子源的屬性
    if (data) {
        std::lock_guard<std::mutex> lock(data->props_mutex);
        for (const auto& prop : data->cached_props) {
            // 使用 child_ 前綴避免與本地屬性衝突
            std::string child_name = "child_" + prop.name;
            
            switch (prop.type) {
            case OBS_PROPERTY_LIST: {
                obs_property_t* list = obs_properties_add_list(props, 
                    child_name.c_str(), prop.description.c_str(),
                    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
                for (const auto& item : prop.items) {
                    obs_property_list_add_string(list, item.first.c_str(), item.second.c_str());
                }
                break;
            }
            case OBS_PROPERTY_BOOL:
                obs_properties_add_bool(props, child_name.c_str(), prop.description.c_str());
                break;
            case OBS_PROPERTY_INT:
                obs_properties_add_int(props, child_name.c_str(), prop.description.c_str(),
                    prop.min_int, prop.max_int, prop.step_int);
                break;
            case OBS_PROPERTY_FLOAT:
                obs_properties_add_float(props, child_name.c_str(), prop.description.c_str(),
                    prop.min_float, prop.max_float, prop.step_float);
                break;
            case OBS_PROPERTY_TEXT:
                obs_properties_add_text(props, child_name.c_str(), prop.description.c_str(),
                    OBS_TEXT_DEFAULT);
                break;
            default:
                break;
            }
        }
    }

    // 備用：如果沒有動態屬性，使用靜態視窗選擇
    if (!data || data->cached_props.empty()) {
        obs_properties_add_list(props, "selected_window",
            "目標視窗 (Target)", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    }

    // 音頻捕捉
    obs_properties_add_bool(props, "audio_capture",
        "音效捕捉 (Audio Capture) [實驗性]");

    return props;
}

static void remote_source_get_defaults(obs_data_t* settings) {
    obs_data_set_default_string(settings, "server_ip", "");
    obs_data_set_default_int(settings, "server_port", DEFAULT_SERVER_PORT);
    obs_data_set_default_int(settings, "capture_mode", CAPTURE_MODE_WINDOW);
    obs_data_set_default_string(settings, "selected_window", "");
    obs_data_set_default_bool(settings, "audio_capture", false);
}

// ========== 來源信息結構 (使用 extern "C" 以便 C 代碼連結) ==========
extern "C" {

struct obs_source_info remote_source_info;

// 初始化函數 - 從 plugin-main.c 中調用
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


