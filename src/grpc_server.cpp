/*
 * grpc_server.cpp
 * gRPC 服務端實現 - Session-based API
 *
 * 功能:
 * 1. GetAvailableSources - 返回可用 source 類型
 * 2. CreateSession/ReleaseSession - Session 管理
 * 3. SetSourceType - 切換 source 類型
 * 4. UpdateSettings - 更新設定並返回刷新屬性
 * 5. StartStream - 綁定 session_id 串流
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <graphics/graphics.h>

// 禁用 protobuf 生成代碼的警告
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4267)  // size_t to int conversion
#pragma warning(disable: 4244)  // possible loss of data
#endif

#include <grpcpp/grpcpp.h>
#include "remote_capture.grpc.pb.h"

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <thread>
#include <atomic>
#include <mutex>
#include <memory>
#include <queue>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>

#include "codec_ffmpeg.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerWriter;
using grpc::Status;
using namespace obsremote;

#include <QMainWindow>
#include <QStatusBar>
#include <QLabel>
#include <QString>

#include "flow_meter.h"

// ========== Session 結構 ==========
struct Session {
    std::string id;
    std::string source_type;
    obs_source_t* capture_source = nullptr;
    std::mutex source_mutex;
    
    // 渲染資源
    gs_texrender_t* texrender = nullptr;
    gs_stagesurf_t* stagesurface = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t frame_number = 0;
    
    // 尺寸變化防抖
    uint32_t pending_width = 0;
    uint32_t pending_height = 0;
    std::chrono::steady_clock::time_point resize_time{};
    static constexpr int RESIZE_DEBOUNCE_MS = 300;
    
    // 編碼器
    std::unique_ptr<FFmpegEncoder> encoder;
    
    // 串流狀態
    std::atomic<bool> streaming{false};
    ServerWriter<StreamFrame>* writer = nullptr;
    std::mutex writer_mutex;
    
    // 音頻隊列
    std::queue<AudioFrame> audio_queue;
    std::mutex audio_mutex;
    
    // 流量計 ID
    uint64_t stream_id{0};
};

// 全局 session 管理
static std::map<std::string, std::unique_ptr<Session>> g_sessions;
static std::mutex g_sessions_mutex;

// ========== 白名單：可用 source 類型 ==========
static const struct {
    const char* id;
    const char* display_name;
} capture_source_whitelist[] = {
    {"window_capture",   "視窗擷取 (Window Capture)"},
    {"game_capture",     "遊戲擷取 (Game Capture)"},
    {"monitor_capture",  "顯示器擷取 (Monitor Capture)"},
    {"display_capture",  "顯示器擷取 (Display Capture)"},
    {"dshow_input",      "視訊擷取裝置 (Video Capture Device)"},
    {"browser_source",   "瀏覽器來源 (Browser Source)"},
    {nullptr, nullptr}
};

// ========== 生成 UUID ==========
static std::string generate_uuid() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    
    std::stringstream ss;
    for (int i = 0; i < 32; i++) {
        if (i == 8 || i == 12 || i == 16 || i == 20) ss << "-";
        ss << std::hex << dis(gen);
    }
    return ss.str();
}

// ========== 從 source 獲取屬性列表 ==========
static void fill_properties_from_source(obs_source_t* source, 
                                         google::protobuf::RepeatedPtrField<Property>* out_props) {
    if (!source) return;
    
    obs_properties_t* props = obs_source_properties(source);
    obs_data_t* defaults = obs_source_get_settings(source);
    
    if (props) {
        obs_property_t* p = obs_properties_first(props);
        while (p) {
            if (!obs_property_visible(p)) {
                obs_property_next(&p);
                continue;
            }
            
            Property* proto_prop = out_props->Add();
            proto_prop->set_name(obs_property_name(p));
            const char* desc = obs_property_description(p);
            proto_prop->set_description(desc ? desc : "");
            proto_prop->set_type(static_cast<PropertyType>(obs_property_get_type(p)));
            proto_prop->set_visible(true);
            
            // 處理 LIST 類型
            if (obs_property_get_type(p) == OBS_PROPERTY_LIST) {
                size_t count = obs_property_list_item_count(p);
                for (size_t i = 0; i < count; i++) {
                    ListItem* item = proto_prop->add_items();
                    const char* item_name = obs_property_list_item_name(p, i);
                    item->set_name(item_name ? item_name : "");
                    
                    if (obs_property_list_format(p) == OBS_COMBO_FORMAT_STRING) {
                        const char* item_val = obs_property_list_item_string(p, i);
                        item->set_value(item_val ? item_val : "");
                    }
                }
                
                const char* name = obs_property_name(p);
                if (name && obs_property_list_format(p) == OBS_COMBO_FORMAT_STRING) {
                    const char* current_val = obs_data_get_string(defaults, name);
                    proto_prop->set_current_string(current_val ? current_val : "");
                }
            }
            
            // 處理 BOOL 類型
            if (obs_property_get_type(p) == OBS_PROPERTY_BOOL) {
                const char* name = obs_property_name(p);
                proto_prop->set_default_bool(obs_data_get_bool(defaults, name));
            }
            
            // 處理 INT 類型
            if (obs_property_get_type(p) == OBS_PROPERTY_INT) {
                proto_prop->set_min_int((int32_t)obs_property_int_min(p));
                proto_prop->set_max_int((int32_t)obs_property_int_max(p));
                proto_prop->set_step_int((int32_t)obs_property_int_step(p));
                proto_prop->set_default_int(
                    (int32_t)obs_data_get_int(defaults, obs_property_name(p)));
            }
            
            // 處理 FLOAT 類型
            if (obs_property_get_type(p) == OBS_PROPERTY_FLOAT) {
                proto_prop->set_min_float(obs_property_float_min(p));
                proto_prop->set_max_float(obs_property_float_max(p));
                proto_prop->set_step_float(obs_property_float_step(p));
                proto_prop->set_default_float(
                    obs_data_get_double(defaults, obs_property_name(p)));
            }
            
            obs_property_next(&p);
        }
        obs_properties_destroy(props);
    }
    
    obs_data_release(defaults);
}

// ========== 音頻捕獲回調 ==========
static void grpc_audio_callback(void* param, obs_source_t* source,
                                 const struct audio_data* audio, bool muted) {
    UNUSED_PARAMETER(source);
    
    Session* session = (Session*)param;
    if (!session || !session->streaming.load() || muted) return;
    if (!audio || audio->frames == 0) return;
    
    // 構建音頻幀
    AudioFrame frame;
    frame.set_sample_rate(48000);
    frame.set_channels(2);
    frame.set_timestamp_ns(os_gettime_ns());
    
    // 交錯立體聲 PCM
    size_t pcm_size = audio->frames * 2 * sizeof(float);
    std::vector<float> interleaved(audio->frames * 2);
    const float* left = (const float*)audio->data[0];
    const float* right = audio->data[1] ? (const float*)audio->data[1] : left;
    
    for (uint32_t i = 0; i < audio->frames; i++) {
        interleaved[i * 2] = left[i];
        interleaved[i * 2 + 1] = right[i];
    }
    
    frame.set_pcm_data(interleaved.data(), pcm_size);
    
    // 加入隊列
    {
        std::lock_guard<std::mutex> lock(session->audio_mutex);
        if (session->audio_queue.size() < 100) {
            session->audio_queue.push(std::move(frame));
        }
    }
}

// ========== 捕獲視頻幀 ==========
static bool capture_video_frame(Session* session, VideoFrame* out_frame) {
    std::lock_guard<std::mutex> lock(session->source_mutex);
    
    if (!session->capture_source || !session->streaming.load()) {
        return false;
    }
    
    uint32_t width = obs_source_get_width(session->capture_source);
    uint32_t height = obs_source_get_height(session->capture_source);
    
    // 每 100 幀輸出一次尺寸資訊
    static uint32_t frame_log_counter = 0;
    if (++frame_log_counter % 100 == 1) {
        blog(LOG_INFO, "[gRPC Server] Session %s: size = %ux%u (frame %u)", 
             session->id.c_str(), width, height, session->frame_number);
    }
    
    if (width == 0 || height == 0) {
        return false;  // 暫停發送，等待 source 就緒
    }
    
    // 尺寸變化處理（帶防抖）
    if (width != session->width || height != session->height) {
        auto now = std::chrono::steady_clock::now();
        
        if (width != session->pending_width || height != session->pending_height) {
            session->pending_width = width;
            session->pending_height = height;
            session->resize_time = now;
            blog(LOG_INFO, "[gRPC Server] Session %s: resize pending %ux%u -> %ux%u",
                 session->id.c_str(), session->width, session->height, width, height);
        }
        
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - session->resize_time).count();
        
        if (elapsed < Session::RESIZE_DEBOUNCE_MS) {
            return false;  // 等待尺寸穩定
        }
        
        // 尺寸穩定，重新創建資源
        blog(LOG_INFO, "[gRPC Server] Session %s: resize stable, recreating resources %ux%u",
             session->id.c_str(), session->pending_width, session->pending_height);
        
        obs_enter_graphics();
        
        if (session->texrender) gs_texrender_destroy(session->texrender);
        if (session->stagesurface) gs_stagesurface_destroy(session->stagesurface);
        
        session->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
        session->stagesurface = gs_stagesurface_create(session->pending_width, session->pending_height, GS_BGRA);
        session->width = session->pending_width;
        session->height = session->pending_height;
        
        obs_leave_graphics();
        
        // 重新初始化編碼器
        if (session->encoder) {
            if (session->encoder->init(session->width, session->height)) {
                blog(LOG_INFO, "[gRPC Server] Session %s: encoder reinitialized %ux%u",
                     session->id.c_str(), session->width, session->height);
            }
        }
    }
    
    if (!session->texrender || !session->stagesurface) {
        return false;
    }
    
    obs_enter_graphics();

    // 渲染源到紋理
    if (gs_texrender_begin(session->texrender, width, height)) {
        struct vec4 clear_color = {0};
        gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
        gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
        
        obs_source_video_render(session->capture_source);
        gs_texrender_end(session->texrender);
    }
    
    // 複製到 staging surface
    gs_texture_t* tex = gs_texrender_get_texture(session->texrender);
    bool success = false;
    
    if (tex) {
        gs_stage_texture(session->stagesurface, tex);
        
        uint8_t* data;
        uint32_t linesize;
        if (gs_stagesurface_map(session->stagesurface, &data, &linesize)) {
            // 使用 H.264 編碼器
            if (session->encoder) {
                std::vector<uint8_t> encoded_data;
                if (session->encoder->encode(data, width, height, linesize, encoded_data)) {
                    out_frame->set_width(width);
                    out_frame->set_height(height);
                    out_frame->set_codec(VideoCodec::CODEC_H264);
                    out_frame->set_frame_data(encoded_data.data(), encoded_data.size());
                    out_frame->set_timestamp_ns(os_gettime_ns());
                    out_frame->set_frame_number(session->frame_number++);
                    success = true;
                }
            }
            gs_stagesurface_unmap(session->stagesurface);
        }
    }
        
    gs_texrender_reset(session->texrender);
    obs_leave_graphics();
    return success;
}

// ========== 服務實現 ==========
class RemoteCaptureServiceImpl final : public RemoteCaptureService::Service {
public:
    // 獲取可用 source 列表
    Status GetAvailableSources(ServerContext* context, 
                               const Empty* request,
                               AvailableSourcesResponse* response) override {
        UNUSED_PARAMETER(context);
        UNUSED_PARAMETER(request);
        
        blog(LOG_INFO, "[gRPC Server] GetAvailableSources");
        
        for (size_t i = 0; capture_source_whitelist[i].id != nullptr; i++) {
            const char* source_id = capture_source_whitelist[i].id;
            
            // 檢查此源類型是否可用
            uint32_t output_flags = obs_get_source_output_flags(source_id);
            if (output_flags == 0) continue;
            
            SourceInfo* info = response->add_sources();
            info->set_id(source_id);
            //TODO: 顯示名稱從系統獲取
            info->set_display_name(capture_source_whitelist[i].display_name);
        }
        
        blog(LOG_INFO, "[gRPC Server] Returned %d available sources", response->sources_size());
        return Status::OK;
    }
    
    // 創建 Session
    Status CreateSession(ServerContext* context,
                         const Empty* request,
                         CreateSessionResponse* response) override {
        UNUSED_PARAMETER(context);
        UNUSED_PARAMETER(request);
        
        std::string session_id = generate_uuid();
        
        {
            std::lock_guard<std::mutex> lock(g_sessions_mutex);
            auto session = std::make_unique<Session>();
            session->id = session_id;
            session->encoder = std::make_unique<FFmpegEncoder>();
            g_sessions[session_id] = std::move(session);
        }
        
        response->set_session_id(session_id);
        blog(LOG_INFO, "[gRPC Server] CreateSession: %s", session_id.c_str());
        return Status::OK;
    }
    
    // 釋放 Session
    Status ReleaseSession(ServerContext* context,
                          const ReleaseSessionRequest* request,
                          Empty* response) override {
        UNUSED_PARAMETER(context);
        UNUSED_PARAMETER(response);
        
        const std::string& session_id = request->session_id();
        blog(LOG_INFO, "[gRPC Server] ReleaseSession: %s", session_id.c_str());
        
        {
            std::lock_guard<std::mutex> lock(g_sessions_mutex);
            auto it = g_sessions.find(session_id);
            if (it != g_sessions.end()) {
                Session* session = it->second.get();
                
                // 停止串流
                session->streaming.store(false);
                
                // 釋放 source
                if (session->capture_source) {
                    obs_source_remove_audio_capture_callback(session->capture_source, 
                        grpc_audio_callback, session);
                    obs_source_dec_active(session->capture_source);
                    obs_source_dec_showing(session->capture_source);
                    obs_source_release(session->capture_source);
                }
                
                // 釋放渲染資源
                obs_enter_graphics();
                if (session->texrender) gs_texrender_destroy(session->texrender);
                if (session->stagesurface) gs_stagesurface_destroy(session->stagesurface);
                obs_leave_graphics();
                
                g_sessions.erase(it);
            }
        }
        
        return Status::OK;
    }
    
    // 設定 Source Type
    Status SetSourceType(ServerContext* context,
                         const SetSourceTypeRequest* request,
                         SetSourceTypeResponse* response) override {
        UNUSED_PARAMETER(context);
        
        const std::string& session_id = request->session_id();
        const std::string& source_type = request->source_type();
        
        blog(LOG_INFO, "[gRPC Server] SetSourceType: session=%s, type=%s", 
             session_id.c_str(), source_type.c_str());
        
        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        auto it = g_sessions.find(session_id);
        if (it == g_sessions.end()) {
            return Status(grpc::StatusCode::NOT_FOUND, "Session not found");
        }
        
        Session* session = it->second.get();
        std::lock_guard<std::mutex> source_lock(session->source_mutex);
        
        // 如果類型相同，不需要重建
        if (session->source_type == source_type && session->capture_source) {
            fill_properties_from_source(session->capture_source, response->mutable_properties());
            return Status::OK;
        }
        
        // 釋放舊 source
        if (session->capture_source) {
            //TODO: 切換soruce這操作，可能要跟streaming loop做好配合，哪怕釋放的一瞬間可能正在做其他事情就會炸掉??
            obs_source_remove_audio_capture_callback(session->capture_source,
                grpc_audio_callback, session);
            obs_source_dec_active(session->capture_source);
            obs_source_dec_showing(session->capture_source);
            obs_source_release(session->capture_source);
            session->capture_source = nullptr;
        }
        
        // 釋放渲染資源
        obs_enter_graphics();
        if (session->texrender) {
            gs_texrender_destroy(session->texrender);
            session->texrender = nullptr;
        }
        if (session->stagesurface) {
            gs_stagesurface_destroy(session->stagesurface);
            session->stagesurface = nullptr;
        }
        obs_leave_graphics();
        session->width = 0;
        session->height = 0;
        
        // 創建新 source
        session->source_type = source_type;
        //TODO: 可以改成: remote_provide_session之類的
        session->capture_source = obs_source_create_private(
            source_type.c_str(), "__grpc_session__", nullptr);
        
        if (!session->capture_source) {
            return Status(grpc::StatusCode::INTERNAL, "Failed to create source");
        }
        
        // 激活源
        obs_source_inc_showing(session->capture_source);
        obs_source_inc_active(session->capture_source);
        
        // 註冊音頻回調
        obs_source_add_audio_capture_callback(session->capture_source, 
            grpc_audio_callback, session);
        
        // 返回屬性
        fill_properties_from_source(session->capture_source, response->mutable_properties());
        
        blog(LOG_INFO, "[gRPC Server] SetSourceType: created source, properties=%d",
             response->properties_size());
        return Status::OK;
    }
    
    // 更新設定
    Status UpdateSettings(ServerContext* context,
                          const UpdateSettingsRequest* request,
                          UpdateSettingsResponse* response) override {
        UNUSED_PARAMETER(context);
        
        const std::string& session_id = request->session_id();
        
        blog(LOG_INFO, "[gRPC Server] UpdateSettings: session=%s, count=%d", 
             session_id.c_str(), request->settings().size());
        
        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        auto it = g_sessions.find(session_id);
        if (it == g_sessions.end()) {
            return Status(grpc::StatusCode::NOT_FOUND, "Session not found");
        }
        
        Session* session = it->second.get();
        std::lock_guard<std::mutex> source_lock(session->source_mutex);
        
        if (!session->capture_source) {
            return Status(grpc::StatusCode::FAILED_PRECONDITION, "No source set");
        }
        
        // 構建設定
        obs_data_t* settings = obs_data_create();
        for (const auto& pair : request->settings()) {
            //TODO: only string???
            obs_data_set_string(settings, pair.first.c_str(), pair.second.c_str());
            blog(LOG_DEBUG, "[gRPC Server]   %s = %s", pair.first.c_str(), pair.second.c_str());
        }
        
        // 更新 source
        obs_source_update(session->capture_source, settings);
        obs_data_release(settings);
        
        // 返回刷新後的屬性
        fill_properties_from_source(session->capture_source, response->mutable_properties());
        
        blog(LOG_INFO, "[gRPC Server] UpdateSettings: refreshed properties=%d",
             response->properties_size());
        return Status::OK;
    }
    
    // 獲取屬性
    Status GetProperties(ServerContext* context,
                         const GetPropertiesRequest* request,
                         GetPropertiesResponse* response) override {
        UNUSED_PARAMETER(context);
        
        const std::string& session_id = request->session_id();
        
        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        auto it = g_sessions.find(session_id);
        if (it == g_sessions.end()) {
            return Status(grpc::StatusCode::NOT_FOUND, "Session not found");
        }
        
        Session* session = it->second.get();
        std::lock_guard<std::mutex> source_lock(session->source_mutex);
        
        if (!session->capture_source) {
            return Status(grpc::StatusCode::FAILED_PRECONDITION, "No source set");
        }
        
        fill_properties_from_source(session->capture_source, response->mutable_properties());
        
        blog(LOG_INFO, "[gRPC Server] GetProperties: session=%s, count=%d",
             session_id.c_str(), response->properties_size());
        return Status::OK;
    }
    
    // 開始串流
    Status StartStream(ServerContext* context,
                       const StartStreamRequest* request,
                       ServerWriter<StreamFrame>* writer) override {
        const std::string& session_id = request->session_id();
        blog(LOG_INFO, "[gRPC Server] StartStream: session=%s", session_id.c_str());
        
        Session* session = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_sessions_mutex);
            auto it = g_sessions.find(session_id);
            if (it == g_sessions.end()) {
                return Status(grpc::StatusCode::NOT_FOUND, "Session not found");
            }
            session = it->second.get();
        }
        
        if (session->streaming.load()) {
            return Status(grpc::StatusCode::ALREADY_EXISTS, "Stream already active");
        }
        
        session->streaming.store(true);
        session->writer = writer;
        session->stream_id = g_flow_meter.registerStream();
        
        blog(LOG_INFO, "[gRPC Server] Stream started for session %s", session_id.c_str());
        
        // 串流迴圈
        while (!context->IsCancelled() && session->streaming.load()) {
            // 發送視頻幀
            VideoFrame video;
            if (capture_video_frame(session, &video)) {
                StreamFrame frame;
                *frame.mutable_video() = video;
                
                std::lock_guard<std::mutex> lock(session->writer_mutex);
                if (writer->Write(frame)) {
                    g_flow_meter.addBytes(session->stream_id, frame.ByteSizeLong());
                } else {
                    blog(LOG_WARNING, "[gRPC Server] Failed to write video frame");
                    break;
                }
            }
            
            // 發送音頻幀
            {
                std::lock_guard<std::mutex> lock(session->audio_mutex);
                while (!session->audio_queue.empty()) {
                    StreamFrame audio_frame;
                    *audio_frame.mutable_audio() = std::move(session->audio_queue.front());
                    session->audio_queue.pop();
                    
                    std::lock_guard<std::mutex> wlock(session->writer_mutex);
                    if (writer->Write(audio_frame)) {
                        g_flow_meter.addBytes(session->stream_id, audio_frame.ByteSizeLong());
                    } else {
                        break;
                    }
                }
            }
            
            // 約 30 FPS
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
        
        // 清理
        session->streaming.store(false);
        session->writer = nullptr;
        g_flow_meter.unregisterStream(session->stream_id);
        
        blog(LOG_INFO, "[gRPC Server] Stream stopped for session %s", session_id.c_str());
        return Status::OK;
    }
};

// ========== 全局狀態 ==========
static std::unique_ptr<Server> g_server;
static std::thread g_server_thread;
static std::atomic<bool> g_running{false};
static std::thread g_tick_thread;
static QLabel* g_status_label = nullptr;

// 狀態欄更新函數 (在 UI 線程調用)
static void UpdateStatusBar(void*) {
    if (!g_status_label) return;
    
    size_t active_count = g_flow_meter.getActiveStreamCount();
    if (active_count > 0) {
        QString rate = g_flow_meter.getFormattedRate();
        QString text = QString("gRPC: %1 stream(s) (%2)")
            .arg(active_count)
            .arg(rate);
        g_status_label->setText(text);
        g_status_label->setStyleSheet("QLabel { color: #00ff00; }");
    } else {
        g_status_label->setText("gRPC: Idle");
        g_status_label->setStyleSheet("QLabel { color: #888888; }");
    }
}

// ========== 公開 API ==========
extern "C" {

void obs_grpc_server_start(void) {
    if (g_running.load()) return;
    
    g_running.store(true);
    
    // 啟動 tick 線程 (每秒更新一次)
    g_tick_thread = std::thread([]() {
        while (g_running.load()) {
            g_flow_meter.tick();
            obs_queue_task(OBS_TASK_UI, UpdateStatusBar, nullptr, false);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });
    
    g_server_thread = std::thread([]() {
        std::string server_address("0.0.0.0:44555");
        RemoteCaptureServiceImpl service;
        
        ServerBuilder builder;
        builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
        builder.RegisterService(&service);
        builder.SetMaxReceiveMessageSize(100 * 1024 * 1024);
        builder.SetMaxSendMessageSize(100 * 1024 * 1024);
        
        g_server = builder.BuildAndStart();
        blog(LOG_INFO, "[gRPC Server] Listening on %s", server_address.c_str());
        
        // 初始化狀態欄 UI (在主線程)
        obs_queue_task(OBS_TASK_UI, [](void*) {
            QMainWindow* main_window = (QMainWindow*)obs_frontend_get_main_window();
            if (main_window) {
                QStatusBar* status_bar = main_window->statusBar();
                if (status_bar) {
                    g_status_label = new QLabel("gRPC: Idle");
                    g_status_label->setStyleSheet("QLabel { color: #888888; margin-right: 10px; }");
                    status_bar->addPermanentWidget(g_status_label);
                    blog(LOG_INFO, "[gRPC Server] Added status bar widget");
                }
            }
        }, nullptr, false);
        
        g_server->Wait();
        blog(LOG_INFO, "[gRPC Server] Stopped");
    });
}

void obs_grpc_server_stop(void) {
    if (!g_running.load()) return;
    
    g_running.store(false);
    
    // 停止 tick 線程
    if (g_tick_thread.joinable()) {
        g_tick_thread.join();
    }
    
    // 釋放所有 session
    {
        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        for (auto& pair : g_sessions) {
            Session* session = pair.second.get();
            session->streaming.store(false);
            
            if (session->capture_source) {
                obs_source_remove_audio_capture_callback(session->capture_source,
                    grpc_audio_callback, session);
                obs_source_dec_active(session->capture_source);
                obs_source_dec_showing(session->capture_source);
                obs_source_release(session->capture_source);
            }
            
            obs_enter_graphics();
            if (session->texrender) gs_texrender_destroy(session->texrender);
            if (session->stagesurface) gs_stagesurface_destroy(session->stagesurface);
            obs_leave_graphics();
        }
        g_sessions.clear();
    }
    
    if (g_server) {
        g_server->Shutdown();
    }
    if (g_server_thread.joinable()) {
        g_server_thread.join();
    }
    g_server.reset();
    
    blog(LOG_INFO, "[gRPC Server] Shutdown complete");
}

}
