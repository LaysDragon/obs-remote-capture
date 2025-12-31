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
#define CAPTURE_MODE_WINDOW 0
#define CAPTURE_MODE_GAME   1
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
    obs_source_t* capture_source;   // 內部的 window/game capture
    
    // 設定
    int capture_mode;               // 0=Window, 1=Game
    std::string selected_window;    // 選定的視窗
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
    
    // 視窗列表緩存
    std::vector<std::pair<std::string, std::string>> window_list;
    
    capture_preview_data() :
        source(nullptr),
        capture_source(nullptr),
        capture_mode(CAPTURE_MODE_WINDOW),
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

// ========== 輔助函數：枚舉視窗 ==========
static void enumerate_windows(capture_preview_data* data) {
    data->window_list.clear();
    
    const char* source_type = (data->capture_mode == CAPTURE_MODE_GAME) 
        ? "game_capture" : "window_capture";
    
    // 創建臨時來源以獲取視窗列表
    obs_source_t* temp_source = obs_source_create(source_type, "__temp_enum__", nullptr, nullptr);
    if (!temp_source) {
        blog(LOG_WARNING, "[Capture Preview] Failed to create temp source for enumeration");
        return;
    }
    
    obs_properties_t* props = obs_source_properties(temp_source);
    if (props) {
        obs_property_t* window_prop = obs_properties_get(props, "window");
        if (window_prop) {
            size_t count = obs_property_list_item_count(window_prop);
            for (size_t i = 0; i < count; i++) {
                const char* name = obs_property_list_item_name(window_prop, i);
                const char* value = obs_property_list_item_string(window_prop, i);
                if (name && value && strlen(value) > 0) {
                    data->window_list.push_back({name, value});
                }
            }
        }
        obs_properties_destroy(props);
    }
    
    obs_source_release(temp_source);
    
    blog(LOG_INFO, "[Capture Preview] Enumerated %zu windows", data->window_list.size());
}

// ========== 創建/重建捕捉源 ==========
static void create_capture_source(capture_preview_data* data) {
    // 釋放舊的捕捉源
    if (data->capture_source) {
        // 先停用舊的源
        obs_source_dec_active(data->capture_source);
        obs_source_dec_showing(data->capture_source);
        obs_source_release(data->capture_source);
        data->capture_source = nullptr;
        blog(LOG_INFO, "[Capture Preview] Released old capture source");
    }
    
    if (data->selected_window.empty()) {
        blog(LOG_INFO, "[Capture Preview] No window selected");
        return;
    }
    
    const char* source_type = (data->capture_mode == CAPTURE_MODE_GAME) 
        ? "game_capture" : "window_capture";
    
    // 創建設定
    obs_data_t* settings = obs_data_create();
    obs_data_set_string(settings, "window", data->selected_window.c_str());
    
    if (data->capture_mode == CAPTURE_MODE_GAME) {
        obs_data_set_int(settings, "mode", 1);  // CAPTURE_MODE_WINDOW
        obs_data_set_bool(settings, "capture_cursor", true);
    } else {
        obs_data_set_bool(settings, "cursor", true);
    }
    
    blog(LOG_INFO, "[Capture Preview] Creating capture source: type=%s, window=%s", 
         source_type, data->selected_window.c_str());
    
    // 創建私有來源
    data->capture_source = obs_source_create_private(source_type, "__capture_preview_internal__", settings);
    obs_data_release(settings);
    
    if (data->capture_source) {
        // 激活私有源 - 這是關鍵！私有源需要手動激活才會開始捕捉
        obs_source_inc_showing(data->capture_source);
        obs_source_inc_active(data->capture_source);
        
        blog(LOG_INFO, "[Capture Preview] Created and ACTIVATED capture source successfully");
    } else {
        blog(LOG_ERROR, "[Capture Preview] Failed to create capture source");
    }
}

// ========== 捕捉線程函數 ==========
static void capture_thread_func(capture_preview_data* data) {
    blog(LOG_INFO, "[Capture Preview] Capture thread started");
    
    uint64_t frame_interval_ns = 1000000000ULL / CAPTURE_FPS;  // 約 33ms
    uint32_t frame_count = 0;
    uint32_t no_source_count = 0;
    uint32_t zero_size_count = 0;
    uint32_t success_count = 0;
    
    while (data->active.load()) {
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

static void capture_preview_update(void* data_ptr, obs_data_t* settings) {
    capture_preview_data* data = (capture_preview_data*)data_ptr;
    
    int new_mode = (int)obs_data_get_int(settings, "capture_mode");
    const char* new_window = obs_data_get_string(settings, "window");
    int new_delay = (int)obs_data_get_int(settings, "delay_ms");
    
    bool need_recreate = (new_mode != data->capture_mode) || 
                         (new_window && data->selected_window != new_window);
    
    data->capture_mode = new_mode;
    data->selected_window = new_window ? new_window : "";
    data->delay_ms = new_delay;
    
    if (need_recreate && data->active.load()) {
        create_capture_source(data);
    }
}

// ========== 刷新視窗列表按鈕回調 ==========
static bool on_refresh_clicked(obs_properties_t* props, obs_property_t* prop, void* data_ptr) {
    UNUSED_PARAMETER(props);
    UNUSED_PARAMETER(prop);
    
    capture_preview_data* data = (capture_preview_data*)data_ptr;
    enumerate_windows(data);
    
    // 需要重新填充視窗列表
    return true;
}

// ========== 模式切換回調 ==========
static bool on_mode_changed(obs_properties_t* props, obs_property_t* prop, obs_data_t* settings) {
    UNUSED_PARAMETER(prop);
    
    capture_preview_data* data = (capture_preview_data*)obs_properties_get_param(props);
    if (data) {
        data->capture_mode = (int)obs_data_get_int(settings, "capture_mode");
        enumerate_windows(data);
    }
    
    return true;
}

static obs_properties_t* capture_preview_get_properties(void* data_ptr) {
    capture_preview_data* data = (capture_preview_data*)data_ptr;
    
    obs_properties_t* props = obs_properties_create();
    obs_properties_set_param(props, data, nullptr);
    
    // 捕捉模式
    obs_property_t* mode_prop = obs_properties_add_list(props, "capture_mode", "Capture Mode",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(mode_prop, "Window Capture", CAPTURE_MODE_WINDOW);
    obs_property_list_add_int(mode_prop, "Game Capture", CAPTURE_MODE_GAME);
    obs_property_set_modified_callback(mode_prop, on_mode_changed);
    
    // 視窗選擇
    obs_property_t* window_prop = obs_properties_add_list(props, "window", "Window",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    
    // 枚舉視窗
    enumerate_windows(data);
    for (const auto& win : data->window_list) {
        obs_property_list_add_string(window_prop, win.first.c_str(), win.second.c_str());
    }
    
    // 刷新按鈕
    obs_properties_add_button(props, "refresh", "Refresh Window List", on_refresh_clicked);
    
    // 延遲設定
    obs_properties_add_int_slider(props, "delay_ms", "Delay (ms)", 0, 5000, 100);
    
    return props;
}

static void capture_preview_get_defaults(obs_data_t* settings) {
    obs_data_set_default_int(settings, "capture_mode", CAPTURE_MODE_WINDOW);
    obs_data_set_default_int(settings, "delay_ms", DEFAULT_DELAY_MS);
}

static void capture_preview_activate(void* data_ptr) {
    capture_preview_data* data = (capture_preview_data*)data_ptr;
    
    if (data->active.load()) return;
    
    blog(LOG_INFO, "[Capture Preview] Activating...");
    
    create_capture_source(data);
    
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
    capture_preview_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
    
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

