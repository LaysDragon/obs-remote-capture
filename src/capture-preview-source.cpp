/*
 * capture-preview-source.cpp
 * 本地擷取預覽來源
 *
 * 功能:
 * 1. 內部調用 window_capture 或 game_capture
 * 2. 將擷取到的畫面延遲後顯示
 * 3. 用於驗證擷取邏輯是否正常工作
 */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/threading.h>
#include <util/platform.h>

#include <thread>
#include <atomic>
#include <mutex>
#include <deque>
#include <vector>
#include <string>
#include <cstring>

// ========== 常量定義 ==========
#define DEFAULT_SOURCE_TYPE "window_capture"
#define DEFAULT_DELAY_MS    2000
#define CAPTURE_FPS         30

// ========== 幀數據結構 ==========
struct FrameData {
    std::vector<uint8_t> pixels;
    uint32_t width;
    uint32_t height;
    uint64_t timestamp_ns;
    
    FrameData() : width(0), height(0), timestamp_ns(0) {}
    FrameData(uint32_t w, uint32_t h) : width(w), height(h), timestamp_ns(0) {
        pixels.resize(w * h * 4);  // BGRA
    }
};

// ========== 來源數據結構 ==========
struct capture_preview_data {
    obs_source_t* source;           // 此來源本身
    obs_source_t* capture_source;   // 內部的 window/game capture (持久化)
    
    // 設定
    std::string source_type;        // 子源類型 ID (如 "window_capture", "game_capture" 等)
    int delay_ms;                   // 延遲毫秒數
    
    // 視頻緩衝 (環形隊列)
    std::deque<FrameData> video_buffer;
    std::mutex buffer_mutex;
    static const size_t MAX_BUFFER_FRAMES = 300;  // 最多緩存 10 秒 (30fps)
    
    // 渲染資源
    gs_texrender_t* texrender;
    gs_stagesurf_t* stagesurface;
    gs_texture_t* output_texture;
    uint32_t output_width;
    uint32_t output_height;
    
    // 捕捉線程
    std::atomic<bool> active;
    std::thread capture_thread;
    
    capture_preview_data() :
        source(nullptr),
        capture_source(nullptr),
        source_type(DEFAULT_SOURCE_TYPE),
        delay_ms(DEFAULT_DELAY_MS),
        texrender(nullptr),
        stagesurface(nullptr),
        output_texture(nullptr),
        output_width(0),
        output_height(0),
        active(false)
    {}
};

// ========== 前向聲明 ==========
static const char* capture_preview_get_name(void* unused);
static void* capture_preview_create(obs_data_t* settings, obs_source_t* source);
static void capture_preview_destroy(void* data);
static void capture_preview_update(void* data, obs_data_t* settings);
static obs_properties_t* capture_preview_get_properties(void* data);
static void capture_preview_get_defaults(obs_data_t* settings);
static void capture_preview_activate(void* data);
static void capture_preview_deactivate(void* data);
static void capture_preview_video_tick(void* data, float seconds);
static void capture_preview_video_render(void* data, gs_effect_t* effect);
static uint32_t capture_preview_get_width(void* data);
static uint32_t capture_preview_get_height(void* data);
static void ensure_capture_source_type(capture_preview_data* data, const char* source_type);
static void audio_capture_callback(void* param, obs_source_t* source, 
                                    const struct audio_data* audio, bool muted);

// 舊的 create_capture_source 已被 ensure_capture_source_type 取代

// ========== 捕捉線程函數 ==========
static void capture_thread_func(capture_preview_data* data) {
    blog(LOG_INFO, "[Capture Preview] Capture thread started");
    
    uint64_t frame_interval_ns = 1000000000ULL / CAPTURE_FPS;  // 約 33ms
    uint32_t frame_count = 0;
    uint32_t no_source_count = 0;
    uint32_t zero_size_count = 0;
    uint32_t success_count = 0;
    
    while (data->active.load()) {
        // os_sleep_ms(100);
        //     continue;
        if (!data->capture_source) {
            no_source_count++;
            if (no_source_count == 1 || no_source_count % 30 == 0) {
                blog(LOG_WARNING, "[Capture Preview] No capture source! (count=%u)", no_source_count);
            }
            os_sleep_ms(100);
            continue;
        }
        no_source_count = 0;
        
        uint64_t start_time = os_gettime_ns();
        
        // 獲取捕捉源的尺寸
        uint32_t width = obs_source_get_width(data->capture_source);
        uint32_t height = obs_source_get_height(data->capture_source);
        
        if (width == 0 || height == 0) {
            zero_size_count++;
            if (zero_size_count == 1 || zero_size_count % 30 == 0) {
                blog(LOG_WARNING, "[Capture Preview] Capture source has zero size! (count=%u, w=%u, h=%u)", 
                     zero_size_count, width, height);
            }
            os_sleep_ms(100);
            continue;
        }
        zero_size_count = 0;
        
        frame_count++;
        
        // 每 30 幀輸出一次調試信息
        bool should_log = (frame_count == 1 || frame_count % 30 == 0);
        
        if (should_log) {
            blog(LOG_INFO, "[Capture Preview] Frame %u: source size = %ux%u", frame_count, width, height);
        }
        
        // 進入圖形上下文
        obs_enter_graphics();
        
        // 確保渲染資源存在且尺寸正確
        if (!data->texrender) {
            data->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
            blog(LOG_INFO, "[Capture Preview] Created texrender: %p", (void*)data->texrender);
        }
        
        if (!data->stagesurface || 
            gs_stagesurface_get_width(data->stagesurface) != width ||
            gs_stagesurface_get_height(data->stagesurface) != height) {
            
            if (data->stagesurface) {
                gs_stagesurface_destroy(data->stagesurface);
            }
            data->stagesurface = gs_stagesurface_create(width, height, GS_BGRA);
            blog(LOG_INFO, "[Capture Preview] Created stagesurface: %p (%ux%u)", 
                 (void*)data->stagesurface, width, height);
        }
        
        // 渲染捕捉源到紋理
        bool render_ok = false;
        if (gs_texrender_begin(data->texrender, width, height)) {
            struct vec4 clear_color = {0};
            gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
            gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
            
            obs_source_video_render(data->capture_source);
            
            gs_texrender_end(data->texrender);
            render_ok = true;
        } else {
            if (should_log) {
                blog(LOG_WARNING, "[Capture Preview] gs_texrender_begin FAILED!");
            }
        }
        
        // 複製紋理到 staging surface
        gs_texture_t* tex = gs_texrender_get_texture(data->texrender);
        if (tex) {
            gs_stage_texture(data->stagesurface, tex);
            
            // 讀取像素數據
            uint8_t* mapped_data;
            uint32_t linesize;
            if (gs_stagesurface_map(data->stagesurface, &mapped_data, &linesize)) {
                FrameData frame(width, height);
                frame.timestamp_ns = os_gettime_ns();
                
                // 複製數據
                for (uint32_t y = 0; y < height; y++) {
                    memcpy(frame.pixels.data() + y * width * 4,
                           mapped_data + y * linesize,
                           width * 4);
                }
                
                gs_stagesurface_unmap(data->stagesurface);
                
                // 加入緩衝區
                {
                    std::lock_guard<std::mutex> lock(data->buffer_mutex);
                    data->video_buffer.push_back(std::move(frame));
                    
                    // 限制緩衝區大小
                    while (data->video_buffer.size() > data->MAX_BUFFER_FRAMES) {
                        data->video_buffer.pop_front();
                    }
                    
                    success_count++;
                    if (should_log) {
                        blog(LOG_INFO, "[Capture Preview] Frame captured! buffer_size=%zu, total_success=%u",
                             data->video_buffer.size(), success_count);
                    }
                }
            } else {
                if (should_log) {
                    blog(LOG_WARNING, "[Capture Preview] gs_stagesurface_map FAILED!");
                }
            }
        } else {
            if (should_log) {
                blog(LOG_WARNING, "[Capture Preview] gs_texrender_get_texture returned NULL! render_ok=%d", render_ok);
            }
        }
        
        gs_texrender_reset(data->texrender);
        obs_leave_graphics();
        
        // 等待下一幀
        uint64_t elapsed = os_gettime_ns() - start_time;
        if (elapsed < frame_interval_ns) {
            os_sleep_ms((uint32_t)((frame_interval_ns - elapsed) / 1000000ULL));
        }
    }
    
    blog(LOG_INFO, "[Capture Preview] Capture thread stopped. Total frames: %u, successful: %u", 
         frame_count, success_count);
}

// ========== 來源回調實現 ==========

static const char* capture_preview_get_name(void* unused) {
    UNUSED_PARAMETER(unused);
    return "Capture Preview (Local Test)";
}

static void* capture_preview_create(obs_data_t* settings, obs_source_t* source) {
    capture_preview_data* data = new capture_preview_data();
    data->source = source;
    
    capture_preview_update(data, settings);
    
    blog(LOG_INFO, "[Capture Preview] Source created");
    return data;
}

static void capture_preview_destroy(void* data_ptr) {
    capture_preview_data* data = (capture_preview_data*)data_ptr;
    
    capture_preview_deactivate(data);
    
    if (data->capture_source) {
        // 移除音頻回調
        obs_source_remove_audio_capture_callback(data->capture_source, audio_capture_callback, data);
        obs_source_release(data->capture_source);
    }
    
    obs_enter_graphics();
    if (data->texrender) {
        gs_texrender_destroy(data->texrender);
    }
    if (data->stagesurface) {
        gs_stagesurface_destroy(data->stagesurface);
    }
    if (data->output_texture) {
        gs_texture_destroy(data->output_texture);
    }
    obs_leave_graphics();
    
    delete data;
    blog(LOG_INFO, "[Capture Preview] Source destroyed");
}

// ========== 從 settings 提取 child_ 開頭的設定 ==========
// 返回的 obs_data_t 需要由調用者釋放
static obs_data_t* extract_child_settings(obs_data_t* settings) {
    obs_data_t* child_settings = obs_data_create();
    obs_data_item_t* item = obs_data_first(settings);
    const char* prefix = "child_";
    const size_t prefix_len = 6;
    
    while (item) {
        const char* key = obs_data_item_get_name(item);
        
        // 只處理 child_ 開頭的屬性
        if (strncmp(key, prefix, prefix_len) == 0) {
            const char* real_key = key + prefix_len;
            enum obs_data_type dtype = obs_data_item_gettype(item);
            
            switch ((int)dtype) {
            case OBS_DATA_STRING:
                obs_data_set_string(child_settings, real_key, obs_data_item_get_string(item));
                break;
            case OBS_DATA_NUMBER:
                if (obs_data_item_numtype(item) == OBS_DATA_NUM_INT) {
                    obs_data_set_int(child_settings, real_key, obs_data_item_get_int(item));
                } else {
                    obs_data_set_double(child_settings, real_key, obs_data_item_get_double(item));
                }
                break;
            case OBS_DATA_BOOLEAN:
                obs_data_set_bool(child_settings, real_key, obs_data_item_get_bool(item));
                break;
            default:
                break;
            }
        }
        obs_data_item_next(&item);
    }
    
    return child_settings;
}

// ========== 前向聲明 (clone_properties_from_child 需要) ==========
static void clone_properties_from_child(obs_properties_t* dest_props, capture_preview_data* data);

// ========== 更新子源並刷新屬性 ==========
static bool update_child_and_refresh_props(obs_properties_t* props, obs_data_t* settings, capture_preview_data* data) {
    if (!data || !data->capture_source) return false;
    // os_sleep_ms(100);
    blog(LOG_INFO, "[Capture Preview] Update child source and refresh properties");
    // 提取並應用子源設定
    obs_data_t* child_settings = extract_child_settings(settings);
    obs_source_update(data->capture_source, child_settings);
    // obs_data_release(child_settings);
    
    // 短暫延遲，讓子源完成 update 的內部處理
    // 避免 obs_source_properties 與子源的內部鎖產生衝突
    os_sleep_ms(1);
    
    // 刷新子源屬性（子源可能因設定變化而更新其可用選項）
    clone_properties_from_child(props, data);
    
    return true;  // 返回 true 刷新 UI
}

static void capture_preview_update(void* data_ptr, obs_data_t* settings) {
    blog(LOG_INFO, "[Capture Preview] Update");
    capture_preview_data* data = (capture_preview_data*)data_ptr;
    
    const char* new_source_type = obs_data_get_string(settings, "source_type");
    int new_delay = (int)obs_data_get_int(settings, "delay_ms");
    
    // 如果沒有設定源類型，使用默認值
    if (!new_source_type || strlen(new_source_type) == 0) {
        new_source_type = DEFAULT_SOURCE_TYPE;
    }
    
    data->source_type = new_source_type;
    data->delay_ms = new_delay;
    
    // 確保子源存在並且類型正確（處理 OBS 重啟後的初始化）
    ensure_capture_source_type(data, new_source_type);
    
    // 翻譯設定並傳遞給子源
    if (data->capture_source) {
        obs_data_t* child_settings = extract_child_settings(settings);
        
        const char* window = obs_data_get_string(child_settings, "window");
        blog(LOG_INFO, "[Capture Preview] Update: source_type=%s, delay=%d, window=%s", 
             data->source_type.c_str(), data->delay_ms, window ? window : "(null)");
        
        obs_source_update(data->capture_source, child_settings);
        obs_data_release(child_settings);
    }
}

static const char* type_to_string(enum obs_data_type type) {
    switch (type) {
    case OBS_DATA_NULL:
        return "OBS_DATA_NULL";
    case OBS_DATA_STRING:
        return "OBS_DATA_STRING";
    case OBS_DATA_NUMBER:
        return "OBS_DATA_NUMBER";
    case OBS_DATA_BOOLEAN:
        return "OBS_DATA_BOOLEAN";
    case OBS_DATA_OBJECT:
        return "OBS_DATA_OBJECT";
    case OBS_DATA_ARRAY:
        return "OBS_DATA_ARRAY";
    default:
        return "Unknown";
    }
}

//TODO: static?
template <typename T>
bool areEqual(const T& a, const T& b) {
    if constexpr (std::is_same_v<T, const char*> || std::is_same_v<T, std::string>) {
        blog(LOG_INFO, "[Capture Preview] areEqual: a=%s, b=%s, result=%d", a, b, strcmp(a, b) == 0);   
        return strcmp(a, b) == 0;
    }
    if constexpr (std::is_same_v<T, int> || std::is_same_v<T, double>) {
        blog(LOG_INFO, "[Capture Preview] areEqual: a=%d, b=%d, result=%d", a, b, a == b);   
        return a == b;
    }
    if constexpr (std::is_same_v<T, bool>) {
        blog(LOG_INFO, "[Capture Preview] areEqual: a=%d, b=%d, result=%d", a, b, a == b);   
        return a == b;
    }
    // not support
    blog(LOG_INFO, "[Capture Preview] areEqual: not support type:%s", typeid(T).name());
    return false;
}

static bool is_data_item_equal(obs_data_t* old_settings,obs_data_t* new_settings,const char * old_item_name,const char * new_item_name){
    obs_data_item_t* old_item = obs_data_item_byname(old_settings, old_item_name);
    obs_data_item_t* new_item = obs_data_item_byname(new_settings, new_item_name);
    enum obs_data_type dtype = obs_data_item_gettype(old_item);
    enum obs_data_type new_dtype = obs_data_item_gettype(new_item);
    blog(LOG_INFO, "[Capture Preview] is_data_item_equal: old_item_name=%s, new_item_name=%s, dtype=%s, new_dtype=%s", old_item_name, new_item_name, type_to_string(dtype), type_to_string(new_dtype));
    if(dtype != new_dtype){
        return false;
    }
            
    switch ((int)dtype) {
        case OBS_DATA_STRING:       
            return  areEqual(obs_data_item_get_string(old_item), obs_data_item_get_string(new_item));
        case OBS_DATA_NUMBER:
            if (obs_data_item_numtype(old_item) == OBS_DATA_NUM_INT) {
                return areEqual(obs_data_item_get_int(old_item), obs_data_item_get_int(new_item));
            } else {
                return areEqual(obs_data_item_get_double(old_item), obs_data_item_get_double(new_item));
            }
        case OBS_DATA_BOOLEAN:
            return areEqual(obs_data_item_get_bool(old_item), obs_data_item_get_bool(new_item));
        default:
        //TODO: not really sure what to do with unsupport type heres
            return true;
    }
}

//TODO: seems broken => Property allow_transparency has no user value,new value=��D��,skip refresh properties!
// what the hell is the garbage output here,seems like pointer problem....
static std::string obs_data_get_any(obs_data_t* settings, const char* key){
    obs_data_item_t* item = obs_data_item_byname(settings, key);
    enum obs_data_type dtype = obs_data_item_gettype(item);
    switch ((int)dtype) {
        case OBS_DATA_NULL:
            return "<null>";
        case OBS_DATA_STRING:       
            return obs_data_item_get_string(item);
        case OBS_DATA_NUMBER:
            if (obs_data_item_numtype(item) == OBS_DATA_NUM_INT) {
                return std::to_string(obs_data_item_get_int(item));
            } else {
                return std::to_string(obs_data_item_get_double(item));
            }
        case OBS_DATA_BOOLEAN:
            return std::to_string(obs_data_item_get_bool(item));
        default:
            return "<not support type>: " + std::to_string(dtype);
    }
}
// ========== 從子源克隆單一屬性 (帶前綴) ==========
#define CHILD_PROP_PREFIX "child_"


static void clone_property(obs_properties_t* dest_props, obs_property_t* src_prop) {
    const char* orig_name = obs_property_name(src_prop);
    const char* desc = obs_property_description(src_prop);
    obs_property_type type = obs_property_get_type(src_prop);
    
    // 創建帶前綴的屬性名稱
    std::string prefixed_name = std::string(CHILD_PROP_PREFIX) + orig_name;
    const char* name = prefixed_name.c_str();
    
    obs_property_t* new_prop = nullptr;
    
    switch (type) {
    case OBS_PROPERTY_BOOL:
        new_prop = obs_properties_add_bool(dest_props, name, desc);
        break;
        
    case OBS_PROPERTY_INT: {
        int min_val = obs_property_int_min(src_prop);
        int max_val = obs_property_int_max(src_prop);
        int step = obs_property_int_step(src_prop);
        obs_number_type num_type = obs_property_int_type(src_prop);
        
        if (num_type == OBS_NUMBER_SLIDER) {
            new_prop = obs_properties_add_int_slider(dest_props, name, desc, min_val, max_val, step);
        } else {
            new_prop = obs_properties_add_int(dest_props, name, desc, min_val, max_val, step);
        }
        break;
    }
    
    case OBS_PROPERTY_FLOAT: {
        double min_val = obs_property_float_min(src_prop);
        double max_val = obs_property_float_max(src_prop);
        double step = obs_property_float_step(src_prop);
        obs_number_type num_type = obs_property_float_type(src_prop);
        
        if (num_type == OBS_NUMBER_SLIDER) {
            new_prop = obs_properties_add_float_slider(dest_props, name, desc, min_val, max_val, step);
        } else {
            new_prop = obs_properties_add_float(dest_props, name, desc, min_val, max_val, step);
        }
        break;
    }
    
    case OBS_PROPERTY_TEXT: {
        obs_text_type text_type = obs_property_text_type(src_prop);
        new_prop = obs_properties_add_text(dest_props, name, desc, text_type);
        obs_property_text_set_info_type(new_prop, obs_property_text_info_type(src_prop));
        break;
    }
    
    case OBS_PROPERTY_LIST: {
        obs_combo_type combo_type = obs_property_list_type(src_prop);
        obs_combo_format format = obs_property_list_format(src_prop);
        new_prop = obs_properties_add_list(dest_props, name, desc, combo_type, format);
        
        // 複製列表選項
        size_t count = obs_property_list_item_count(src_prop);
        for (size_t i = 0; i < count; i++) {
            const char* item_name = obs_property_list_item_name(src_prop, i);
            
            if (format == OBS_COMBO_FORMAT_INT) {
                long long item_val = obs_property_list_item_int(src_prop, i);
                obs_property_list_add_int(new_prop, item_name, item_val);
            } else if (format == OBS_COMBO_FORMAT_FLOAT) {
                double item_val = obs_property_list_item_float(src_prop, i);
                obs_property_list_add_float(new_prop, item_name, item_val);
            } else {
                const char* item_val = obs_property_list_item_string(src_prop, i);
                obs_property_list_add_string(new_prop, item_name, item_val ? item_val : "");
            }
        }
        break;
    }
    
    case OBS_PROPERTY_PATH: {
        obs_path_type path_type = obs_property_path_type(src_prop);
        const char* filter = obs_property_path_filter(src_prop);
        const char* default_path = obs_property_path_default_path(src_prop);
        new_prop = obs_properties_add_path(dest_props, name, desc, path_type, filter, default_path);
        break;
    }
    
    case OBS_PROPERTY_BUTTON:
        // 按鈕無法有效複製 (回調函數無法複製)，跳過
        blog(LOG_DEBUG, "[Capture Preview] Skipping button property: %s", orig_name);
        break;
        
    case OBS_PROPERTY_COLOR:
        new_prop = obs_properties_add_color(dest_props, name, desc);
        break;
        
    case OBS_PROPERTY_COLOR_ALPHA:
        new_prop = obs_properties_add_color_alpha(dest_props, name, desc);
        break;
        
    default:
        blog(LOG_DEBUG, "[Capture Preview] Unknown property type %d for: %s", type, orig_name);
        break;
    }
    
    // 複製可見性並添加回調
    if (new_prop) {
        obs_property_set_visible(new_prop, obs_property_visible(src_prop));
        obs_property_set_enabled(new_prop, obs_property_enabled(src_prop));
        obs_property_set_long_description(new_prop, obs_property_long_description(src_prop));
        
        // 對所有 child 屬性添加級聯刷新回調
        // 當任何 child 屬性變化時，更新子源並刷新屬性列表
        obs_property_set_modified_callback(new_prop, 
            [](obs_properties_t* props, obs_property_t* p, obs_data_t* settings) -> bool {
                // UNUSED_PARAMETER(p);
                capture_preview_data* data = (capture_preview_data*)obs_properties_get_param(props);
                obs_data_t* old_settings = obs_source_get_settings(data->capture_source);

                // 只有模式真正改變時才重建
                //TODO: 考慮到首次初始化時是否有有效的settings可供比對，為何初始更新就會觸發這個modified，而創建新的好樣就不會
                //TODO: 另外capture source的settings name mpaaing與上層nameing mapping不一樣，這裡要重新映射才行，要拿掉child_前綴
                const char* old_prop_name = obs_property_name(p);
                const char* new_prop_name = obs_property_name(p);
                const char* real_child_key = old_prop_name + strlen("child_");
                if(!obs_data_has_user_value(old_settings, real_child_key)) {
                    //log property new settting value
                    blog(LOG_INFO, "[Capture Preview] Property %s has no user value,new value=%s,skip refresh properties!", real_child_key, obs_data_get_any(settings, real_child_key));
                    return false;
                }
                
                if (is_data_item_equal(old_settings, settings, real_child_key,new_prop_name)) {
                    blog(LOG_INFO, "[Capture Preview] Property %s has no change,skip refresh properties!", real_child_key);
                    return false;
                }
                return update_child_and_refresh_props(props, settings, data);
            });
    }
}

// ========== 音頻捕獲回調 - 轉發子源音頻到父源 ==========
static void audio_capture_callback(void* param, obs_source_t* source, 
                                    const struct audio_data* audio, bool muted) {
    UNUSED_PARAMETER(source);
    UNUSED_PARAMETER(muted);
    
    capture_preview_data* data = (capture_preview_data*)param;
    if (data->source && audio && audio->frames > 0) {
        // 將子源的音頻轉發到父源
        struct obs_source_audio out_audio = {};
        for (size_t i = 0; i < MAX_AV_PLANES && audio->data[i]; i++) {
            out_audio.data[i] = audio->data[i];
        }
        out_audio.frames = audio->frames;
        out_audio.speakers = SPEAKERS_STEREO;  // 假設立體聲
        out_audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
        out_audio.samples_per_sec = 48000;  // 假設 48kHz
        out_audio.timestamp = audio->timestamp;
        
        obs_source_output_audio(data->source, &out_audio);
    }
}

// ========== 確保子源存在且類型正確 ==========
static void ensure_capture_source_type(capture_preview_data* data, const char* source_type) {
    if (!source_type || strlen(source_type) == 0) {
        blog(LOG_WARNING, "[Capture Preview] Invalid source type");
        return;
    }
    
    // 如果已有子源且類型匹配，不需要重建
    if (data->capture_source) {
        const char* current_type = obs_source_get_id(data->capture_source);
        if (strcmp(current_type, source_type) == 0) {
            return;  // 類型相同，不需要重建
        }
        
        // 類型不同，需要銷毀舊的
        blog(LOG_INFO, "[Capture Preview] Changing source type from %s to %s", current_type, source_type);
        
        // 移除音頻回調
        obs_source_remove_audio_capture_callback(data->capture_source, audio_capture_callback, data);
        
        obs_source_dec_active(data->capture_source);
        obs_source_dec_showing(data->capture_source);
        obs_source_release(data->capture_source);
        data->capture_source = nullptr;
    }
    
    // 創建新的子源
    blog(LOG_INFO, "[Capture Preview] Creating persistent child source: %s", source_type);
    data->capture_source = obs_source_create_private(source_type, "__capture_preview_internal__", nullptr);
    
    if (data->capture_source) {
        // 激活子源
        obs_source_inc_showing(data->capture_source);
        obs_source_inc_active(data->capture_source);
        
        // 註冊音頻捕獲回調
        obs_source_add_audio_capture_callback(data->capture_source, audio_capture_callback, data);
        
        blog(LOG_INFO, "[Capture Preview] Created and activated child source with audio: %s", source_type);
    } else {
        blog(LOG_ERROR, "[Capture Preview] Failed to create child source: %s", source_type);
    }
}

// ========== 從持久化子源克隆屬性 ==========
static void clone_properties_from_child(obs_properties_t* dest_props, capture_preview_data* data) {
    if (!data->capture_source) {
        blog(LOG_WARNING, "[Capture Preview] No child source for property cloning");
        return;
    }
    
    // ===== 首先移除所有舊的 child_ 屬性 =====
    std::vector<std::string> props_to_remove;
    obs_property_t* prop = obs_properties_first(dest_props);
    while (prop) {
        const char* name = obs_property_name(prop);
        if (strncmp(name, CHILD_PROP_PREFIX, strlen(CHILD_PROP_PREFIX)) == 0) {
            props_to_remove.push_back(name);
        }
        obs_property_next(&prop);
    }
    
    for (const auto& name : props_to_remove) {
        obs_properties_remove_by_name(dest_props, name.c_str());
    }
    blog(LOG_INFO, "[Capture Preview] Removed %d old child properties", (int)props_to_remove.size());
    
    // ===== 從子源獲取並克隆新屬性 =====
    obs_properties_t* src_props = obs_source_properties(data->capture_source);
    if (!src_props) {
        blog(LOG_WARNING, "[Capture Preview] Failed to get properties from child source");
        return;
    }
    
    // 遍歷所有屬性並克隆
    prop = obs_properties_first(src_props);
    int prop_count = 0;
    while (prop) {
        clone_property(dest_props, prop);
        prop_count++;
        obs_property_next(&prop);
    }
    
    blog(LOG_INFO, "[Capture Preview] Cloned %d properties from child source", prop_count);
    
    obs_properties_destroy(src_props);
}

// ========== 延遲更新屬性的回調函數 ==========
static void deferred_update_properties(void* param) {
    obs_source_t* source = (obs_source_t*)param;
    if (source) {
        blog(LOG_INFO, "[Capture Preview] Deferred obs_source_update_properties called");
        obs_source_update_properties(source);
        // obs_source_release(source);  // 釋放我們增加的引用
    }
}

// ========== 源類型切換回調 ==========
static bool on_source_type_changed(obs_properties_t* props, obs_property_t* prop, obs_data_t* settings) {
    UNUSED_PARAMETER(prop);
    
    capture_preview_data* data = (capture_preview_data*)obs_properties_get_param(props);
    if (!data) return false;
    
    const char* new_type = obs_data_get_string(settings, "source_type");
    const char* old_type = data->source_type.c_str();
    
    // 只有類型真正改變時才重建
    if (!new_type || strlen(new_type) == 0) {
        return false;
    }
    
    if (strcmp(new_type, old_type) != 0) {
        data->source_type = new_type;
        
        // 確保子源類型正確
        ensure_capture_source_type(data, new_type);
        
        blog(LOG_INFO, "[Capture Preview] Source type changed from %s to %s", old_type, new_type);
        
        // 重新克隆子源的屬性
        clone_properties_from_child(props, data);
        
        // ===== DEBUG: 列出最終的所有屬性 =====
        blog(LOG_INFO, "[Capture Preview] === Final properties list (on_source_type_changed) ===");
        obs_property_t* debug_prop = obs_properties_first(props);
        int total_count = 0;
        while (debug_prop) {
            const char* prop_name = obs_property_name(debug_prop);
            blog(LOG_INFO, "[Capture Preview]   [%d] %s", total_count, prop_name);
            total_count++;
            obs_property_next(&debug_prop);
        }
        blog(LOG_INFO, "[Capture Preview] === Total: %d properties ===", total_count);
        
        return true;  // 返回 true，讓 OBS 刷新 UI
    }
    return false;
}
// 白名單：只列出捕捉相關的源類型
static const char* capture_source_whitelist[] = {
    "window_capture",       // 視窗擷取
    "game_capture",         // 遊戲擷取
    "monitor_capture",      // 顯示器擷取 (Mac)
    "display_capture",      // 顯示器擷取 (Windows)
    "dshow_input",          // 視訊擷取裝置 (DirectShow)
    "v4l2_input",           // 視訊擷取裝置 (Linux V4L2)
    "av_capture_input",     // 視訊擷取裝置 (Mac)
    "browser_source",       // 瀏覽器來源
    "ndi_source",           // NDI 源 (如果安裝了 NDI 插件)
    "obs-ndi-source",       // NDI 源 (另一種 ID)
    "pipewire-desktop-capture-source",  // PipeWire 桌面擷取 (Linux)
    "xshm_input",           // X11 擷取 (Linux)
    "xcomposite_input",     // X11 Composite 擷取 (Linux)
    nullptr  // 結束標記
};

// 獲取可用的捕捉源列表 (返回 id 和顯示名稱對)
static std::vector<std::pair<std::string, std::string>> get_available_capture_sources() {
    std::vector<std::pair<std::string, std::string>> sources;
    
    for (size_t i = 0; capture_source_whitelist[i] != nullptr; i++) {
        const char* source_id = capture_source_whitelist[i];
        
        // 檢查此源類型是否可用（已註冊）
        uint32_t output_flags = obs_get_source_output_flags(source_id);
        if (output_flags == 0) {
            continue;  // 此源類型不存在
        }
        
        // 獲取源類型的顯示名稱
        const char* display_name = obs_source_get_display_name(source_id);
        std::string name = (display_name && strlen(display_name) > 0) ? display_name : source_id;
        
        sources.emplace_back(source_id, name);
    }
    
    return sources;
}

static obs_properties_t* capture_preview_get_properties(void* data_ptr) {
    blog(LOG_INFO, "[Capture Preview] Getting properties");
    capture_preview_data* data = (capture_preview_data*)data_ptr;
    
    // 確保子源存在
    ensure_capture_source_type(data, data->source_type.c_str());
    
    obs_properties_t* props = obs_properties_create();
    obs_properties_set_param(props, data, nullptr);
    
    // ===== 延遲設定 (放在最上面) =====
    obs_properties_add_int_slider(props, "delay_ms", "Preview Delay (ms)", 0, 5000, 100);
    
    // ===== 源類型選擇 (白名單) =====
    obs_property_t* type_prop = obs_properties_add_list(props, "source_type", "Source Type",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    
    // 添加可用的源類型
    auto available_sources = get_available_capture_sources();
    for (const auto& source : available_sources) {
        obs_property_list_add_string(type_prop, source.second.c_str(), source.first.c_str());
    }
    
    obs_property_set_modified_callback(type_prop, on_source_type_changed);
    
    // ===== 分隔線/標題 =====
    obs_properties_add_text(props, "child_props_label", 
        "--- Wrapped Source Settings ---", OBS_TEXT_INFO);
    
    // ===== 從持久化子源克隆屬性 =====
    clone_properties_from_child(props, data);
    
    // ===== DEBUG: 列出最終的所有屬性 =====
    blog(LOG_INFO, "[Capture Preview] === Final properties list (get_properties) ===");
    obs_property_t* debug_prop = obs_properties_first(props);
    int total_count = 0;
    while (debug_prop) {
        const char* prop_name = obs_property_name(debug_prop);
        blog(LOG_INFO, "[Capture Preview]   [%d] %s", total_count, prop_name);
        total_count++;
        obs_property_next(&debug_prop);
    }
    blog(LOG_INFO, "[Capture Preview] === Total: %d properties ===", total_count);
    
    return props;
}

static void capture_preview_get_defaults(obs_data_t* settings) {
    obs_data_set_default_string(settings, "source_type", DEFAULT_SOURCE_TYPE);
    obs_data_set_default_int(settings, "delay_ms", DEFAULT_DELAY_MS);
}

static void capture_preview_activate(void* data_ptr) {
    capture_preview_data* data = (capture_preview_data*)data_ptr;
    
    if (data->active.load()) return;
    
    blog(LOG_INFO, "[Capture Preview] Activating...");
    
    // 確保子源存在（如果還沒創建的話）
    ensure_capture_source_type(data, data->source_type.c_str());
    
    data->active.store(true);
    data->capture_thread = std::thread(capture_thread_func, data);
}

static void capture_preview_deactivate(void* data_ptr) {
    capture_preview_data* data = (capture_preview_data*)data_ptr;
    
    if (!data->active.load()) return;
    
    blog(LOG_INFO, "[Capture Preview] Deactivating...");
    
    data->active.store(false);
    
    if (data->capture_thread.joinable()) {
        data->capture_thread.join();
    }
    
    // 清空緩衝區
    {
        std::lock_guard<std::mutex> lock(data->buffer_mutex);
        data->video_buffer.clear();
    }
}

static void capture_preview_video_tick(void* data_ptr, float seconds) {
    UNUSED_PARAMETER(seconds);
    capture_preview_data* data = (capture_preview_data*)data_ptr;
    
    static uint32_t tick_count = 0;
    tick_count++;
    bool should_log = (tick_count == 1 || tick_count % 60 == 0);  // 每 60 tick (~1秒) 輸出一次
    
    if (!data->active.load()) {
        if (should_log) {
            blog(LOG_DEBUG, "[Capture Preview] video_tick: not active");
        }
        return;
    }
    
    // 從緩衝區中找到延遲後應該顯示的幀
    uint64_t now = os_gettime_ns();
    uint64_t delay_ns = (uint64_t)data->delay_ms * 1000000ULL;
    uint64_t target_time = now - delay_ns;
    
    FrameData* display_frame = nullptr;
    size_t buffer_size = 0;
    
    {
        std::lock_guard<std::mutex> lock(data->buffer_mutex);
        buffer_size = data->video_buffer.size();
        
        // 找到時間戳小於等於目標時間的最新幀
        for (auto it = data->video_buffer.rbegin(); it != data->video_buffer.rend(); ++it) {
            if (it->timestamp_ns <= target_time) {
                display_frame = &(*it);
                break;
            }
        }
    }
    
    if (should_log) {
        blog(LOG_INFO, "[Capture Preview] video_tick: buffer_size=%zu, delay=%dms, found_frame=%s",
             buffer_size, data->delay_ms, display_frame ? "YES" : "NO");
    }
    
    if (display_frame && display_frame->width > 0 && display_frame->height > 0) {
        // 更新輸出紋理
        obs_enter_graphics();
        
        if (!data->output_texture ||
            data->output_width != display_frame->width ||
            data->output_height != display_frame->height) {
            
            if (data->output_texture) {
                gs_texture_destroy(data->output_texture);
            }
            data->output_texture = gs_texture_create(
                display_frame->width, display_frame->height, 
                GS_BGRA, 1, nullptr, GS_DYNAMIC);
            data->output_width = display_frame->width;
            data->output_height = display_frame->height;
            
            blog(LOG_INFO, "[Capture Preview] Created output texture: %p (%ux%u)",
                 (void*)data->output_texture, data->output_width, data->output_height);
        }
        
        if (data->output_texture) {
            gs_texture_set_image(data->output_texture, 
                display_frame->pixels.data(), 
                display_frame->width * 4, false);
        }
        
        obs_leave_graphics();
    }
}

static void capture_preview_video_render(void* data_ptr, gs_effect_t* effect) {
    capture_preview_data* data = (capture_preview_data*)data_ptr;
    
    if (!data->output_texture) return;
    
    gs_effect_t* default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_technique_t* tech = gs_effect_get_technique(default_effect, "Draw");
    
    gs_technique_begin(tech);
    gs_technique_begin_pass(tech, 0);
    
    gs_effect_set_texture(gs_effect_get_param_by_name(default_effect, "image"), 
                          data->output_texture);
    
    gs_draw_sprite(data->output_texture, 0, data->output_width, data->output_height);
    
    gs_technique_end_pass(tech);
    gs_technique_end(tech);
}

static uint32_t capture_preview_get_width(void* data_ptr) {
    capture_preview_data* data = (capture_preview_data*)data_ptr;
    return data->output_width;
}

static uint32_t capture_preview_get_height(void* data_ptr) {
    capture_preview_data* data = (capture_preview_data*)data_ptr;
    return data->output_height;
}

// ========== 來源信息結構 ==========
extern "C" {

struct obs_source_info capture_preview_info;

void init_capture_preview_info() {
    memset(&capture_preview_info, 0, sizeof(capture_preview_info));
    
    capture_preview_info.id = "capture_preview";
    capture_preview_info.type = OBS_SOURCE_TYPE_INPUT;
    capture_preview_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW;
    
    capture_preview_info.get_name = capture_preview_get_name;
    capture_preview_info.create = capture_preview_create;
    capture_preview_info.destroy = capture_preview_destroy;
    capture_preview_info.update = capture_preview_update;
    capture_preview_info.get_properties = capture_preview_get_properties;
    capture_preview_info.get_defaults = capture_preview_get_defaults;
    capture_preview_info.activate = capture_preview_activate;
    capture_preview_info.deactivate = capture_preview_deactivate;
    capture_preview_info.video_tick = capture_preview_video_tick;
    capture_preview_info.video_render = capture_preview_video_render;
    capture_preview_info.get_width = capture_preview_get_width;
    capture_preview_info.get_height = capture_preview_get_height;
}

}  // extern "C"

