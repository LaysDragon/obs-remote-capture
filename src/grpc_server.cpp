/*
 * grpc_server.cpp
 * gRPC 服務端實現
 *
 * 功能:
 * 1. 提供 GetProperties RPC - 返回捕捉源屬性
 * 2. 提供 StartStream RPC - 串流影音數據
 * 3. 提供 StopStream RPC - 停止串流
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

// ========== 串流會話狀態 ==========
struct GrpcStreamSession {
    obs_source_t* capture_source = nullptr;
    std::atomic<bool> active{false};
    ServerWriter<StreamFrame>* writer = nullptr;
    std::mutex writer_mutex;
    
    // 渲染相關
    gs_texrender_t* texrender = nullptr;
    gs_stagesurf_t* stagesurface = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t frame_number = 0;
    
    // H.264 編碼器
    std::unique_ptr<FFmpegEncoder> encoder;

    // 音頻隊列
    std::queue<AudioFrame> audio_queue;
    std::mutex audio_mutex;
    
    // 流量計 ID
    uint64_t stream_id{0};
};

// ========== 音頻捕獲回調 ==========
static void grpc_audio_callback(void* param, obs_source_t* source,
                                 const struct audio_data* audio, bool muted) {
    UNUSED_PARAMETER(source);
    
    GrpcStreamSession* session = (GrpcStreamSession*)param;
    if (!session || !session->active.load() || muted) return;
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
        if (session->audio_queue.size() < 100) {  // 限制隊列大小
            session->audio_queue.push(std::move(frame));
        }
    }
}

// ========== 捕獲視頻幀 ==========
static bool capture_video_frame(GrpcStreamSession* session, VideoFrame* out_frame) {
    if (!session || !session->capture_source || !session->active.load()) {
        blog(LOG_DEBUG, "[gRPC Server] capture_video_frame: session invalid or inactive");
        return false;
    }
    
    uint32_t width = obs_source_get_width(session->capture_source);
    uint32_t height = obs_source_get_height(session->capture_source);
    
    // 每 100 幀輸出一次尺寸資訊
    static uint32_t frame_log_counter = 0;
    if (++frame_log_counter % 100 == 1) {
        blog(LOG_INFO, "[gRPC Server] Source size: %ux%u (frame %u)", 
             width, height, session->frame_number);
    }
    
    if (width == 0 || height == 0) {
        if (frame_log_counter % 30 == 1) {
            blog(LOG_WARNING, "[gRPC Server] Source size is 0x0, source may not be active");
        }
        return false;
    }
    
    // 重新創建渲染資源 (尺寸變化時)
    if (width != session->width || height != session->height) {
        blog(LOG_INFO, "[gRPC Server] Recreating render resources: %ux%u -> %ux%u",
             session->width, session->height, width, height);
        
        obs_enter_graphics();
        
        if (session->texrender) gs_texrender_destroy(session->texrender);
        if (session->stagesurface) gs_stagesurface_destroy(session->stagesurface);
        
        session->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
        session->stagesurface = gs_stagesurface_create(width, height, GS_BGRA);
        session->width = width;
        session->height = height;
        
        blog(LOG_INFO, "[gRPC Server] Created texrender=%p, stagesurface=%p",
             (void*)session->texrender, (void*)session->stagesurface);
        
        obs_leave_graphics();
        
        // 初始化/重新初始化 H.264 編碼器
        if (session->encoder) {
            if (session->encoder->init(width, height)) {
                blog(LOG_INFO, "[gRPC Server] H.264 encoder initialized: %s (%ux%u)",
                     session->encoder->getName(), width, height);
            } else {
                blog(LOG_ERROR, "[gRPC Server] Failed to init H.264 encoder");
            }
        }
    }
    
    if (!session->texrender || !session->stagesurface) {
        blog(LOG_WARNING, "[gRPC Server] texrender or stagesurface is null");
        return false;
    }
    
    obs_enter_graphics();

    // 渲染源到紋理
    bool render_success = false;
    if (gs_texrender_begin(session->texrender, width, height)) {
        struct vec4 clear_color = {0};
        gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
        gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
        
        obs_source_video_render(session->capture_source);
        gs_texrender_end(session->texrender);
        render_success = true;
    } else {
        blog(LOG_WARNING, "[gRPC Server] gs_texrender_begin failed");
    }
    
    // 複製到 staging surface
    gs_texture_t* tex = gs_texrender_get_texture(session->texrender);
    bool success = false;
    
    if (!tex) {
        blog(LOG_WARNING, "[gRPC Server] gs_texrender_get_texture returned null");
    } else {
        gs_stage_texture(session->stagesurface, tex);
        
        uint8_t* data;
        uint32_t linesize;
        if (gs_stagesurface_map(session->stagesurface, &data, &linesize)) {
            // 使用 FFmpeg H.264 編碼器
            if (session->encoder) {
                std::vector<uint8_t> encoded_data;
                bool is_keyframe = false;
                
                if (session->encoder->encode(data, width, height, linesize, 
                                              encoded_data, is_keyframe)) {
                    out_frame->set_width(width);
                    out_frame->set_height(height);
                    out_frame->set_codec(VideoCodec::CODEC_H264);
                    out_frame->set_frame_data(encoded_data.data(), encoded_data.size());
                    out_frame->set_timestamp_ns(os_gettime_ns());
                    out_frame->set_frame_number(session->frame_number++);
                    out_frame->set_is_keyframe(is_keyframe);
                    
                    // 發送 SPS/PPS (keyframe 時)
                    if (is_keyframe && !session->encoder->getExtraData().empty()) {
                        const auto& extra = session->encoder->getExtraData();
                        out_frame->set_sps_pps(extra.data(), extra.size());
                    }
                    
                    success = true;
                    
                    // 每 100 幀輸出一次壓縮資訊
                    if (session->frame_number % 100 == 0) {
                        blog(LOG_INFO, "[gRPC Server] H.264 frame=%u, encoder=%s, size=%zu, keyframe=%d",
                             session->frame_number, session->encoder->getName(), 
                             encoded_data.size(), is_keyframe);
                    }
                } else {
                    blog(LOG_WARNING, "[gRPC Server] H.264 encode failed");
                }
            } else {
                blog(LOG_WARNING, "[gRPC Server] No encoder available");
            }
            gs_stagesurface_unmap(session->stagesurface);
        } else {
            blog(LOG_WARNING, "[gRPC Server] gs_stagesurface_map failed");
        }
    }
        
    gs_texrender_reset(session->texrender);
    obs_leave_graphics();
    return success;
}

// ========== 服務實現 ==========
class RemoteCaptureServiceImpl final : public RemoteCaptureService::Service {
public:
    // 獲取屬性列表
    Status GetProperties(ServerContext* context, 
                         const GetPropertiesRequest* request,
                         GetPropertiesResponse* response) override {
        UNUSED_PARAMETER(context);
        
        const char* source_type = request->source_type().c_str();
        blog(LOG_INFO, "[gRPC Server] GetProperties: %s", source_type);
        
        // 創建臨時源獲取屬性
        obs_source_t* temp_source = obs_source_create(source_type, "__temp_grpc__", nullptr, nullptr);
        if (!temp_source) {
            return Status(grpc::StatusCode::NOT_FOUND, "Source type not found");
        }
        
        obs_properties_t* props = obs_source_properties(temp_source);
        obs_data_t* defaults = obs_source_get_settings(temp_source);
        
        if (props) {
            obs_property_t* p = obs_properties_first(props);
            while (p) {
                if (!obs_property_visible(p)) {
                    obs_property_next(&p);
                    continue;
                }
                
                Property* proto_prop = response->add_properties();
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
        obs_source_release(temp_source);
        
        blog(LOG_INFO, "[gRPC Server] Returned %d properties", response->properties_size());
        return Status::OK;
    }
    
    // 開始串流
    Status StartStream(ServerContext* context,
                       const StartStreamRequest* request,
                       ServerWriter<StreamFrame>* writer) override {
        const std::string& source_type = request->source_type();
        blog(LOG_INFO, "[gRPC Server] StartStream: %s", source_type.c_str());
        
        // 創建設定
        obs_data_t* settings = obs_data_create();
        for (const auto& pair : request->settings()) {
            obs_data_set_string(settings, pair.first.c_str(), pair.second.c_str());
        }
        
        // 創建捕捉源
        obs_source_t* capture_source = obs_source_create_private(
            source_type.c_str(), "__grpc_capture__", settings);
        obs_data_release(settings);
        
        if (!capture_source) {
            return Status(grpc::StatusCode::INTERNAL, "Failed to create capture source");
        }
        
        // 創建會話
        GrpcStreamSession session;
        session.capture_source = capture_source;
        session.active.store(true);
        session.writer = writer;
        
        // 創建 H.264 編碼器 (會在第一幀時初始化)
        session.encoder = std::make_unique<FFmpegEncoder>();
        blog(LOG_INFO, "[gRPC Server] FFmpeg encoder created (init on first frame)");
        
        // 激活源
        obs_source_inc_showing(capture_source);
        obs_source_inc_active(capture_source);
        
        // 註冊音頻回調
        obs_source_add_audio_capture_callback(capture_source, grpc_audio_callback, &session);
        
        blog(LOG_INFO, "[gRPC Server] Stream started");
        
        // 註冊到流量計
        session.stream_id = g_flow_meter.registerStream();
        
        // 串流循環
        while (!context->IsCancelled() && session.active.load()) {
            StreamFrame frame;
            
            // 發送視頻幀
            VideoFrame video;
            if (capture_video_frame(&session, &video)) {
                *frame.mutable_video() = video;
                
                std::lock_guard<std::mutex> lock(session.writer_mutex);
                if (writer->Write(frame)) {
                    g_flow_meter.addBytes(session.stream_id, frame.ByteSizeLong());
                } else {
                    blog(LOG_WARNING, "[gRPC Server] Failed to write video frame");
                    break;
                }
            }
            
            // 發送音頻幀
            {
                std::lock_guard<std::mutex> lock(session.audio_mutex);
                while (!session.audio_queue.empty()) {
                    StreamFrame audio_frame;
                    *audio_frame.mutable_audio() = std::move(session.audio_queue.front());
                    session.audio_queue.pop();
                    
                    std::lock_guard<std::mutex> wlock(session.writer_mutex);
                    if (writer->Write(audio_frame)) {
                         g_flow_meter.addBytes(session.stream_id, audio_frame.ByteSizeLong());
                    } else {
                        blog(LOG_WARNING, "[gRPC Server] Failed to write audio frame");
                        break;
                    }
                }
            }
            
            // 約 30 FPS
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
        
        // 清理
        session.active.store(false);
        
        // 從流量計註銷
        g_flow_meter.unregisterStream(session.stream_id);
        
        obs_source_remove_audio_capture_callback(capture_source, grpc_audio_callback, &session);
        obs_source_dec_active(capture_source);
        obs_source_dec_showing(capture_source);
        
        obs_enter_graphics();
        if (session.texrender) gs_texrender_destroy(session.texrender);
        if (session.stagesurface) gs_stagesurface_destroy(session.stagesurface);
        obs_leave_graphics();
        
        // encoder 由 unique_ptr 自動清理
        session.encoder.reset();
        
        obs_source_release(capture_source);
        
        blog(LOG_INFO, "[gRPC Server] Stream stopped");
        return Status::OK;
    }
    
    // 停止串流
    Status StopStream(ServerContext* context,
                      const StopStreamRequest* request,
                      StopStreamResponse* response) override {
        UNUSED_PARAMETER(context);
        UNUSED_PARAMETER(request);
        
        blog(LOG_INFO, "[gRPC Server] StopStream");
        response->set_success(true);
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
            // 更新流量計速率
            g_flow_meter.tick();
            
            // 在 UI 線程更新狀態欄
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
    
    if (g_server) {
        g_server->Shutdown();
    }
    if (g_server_thread.joinable()) {
        g_server_thread.join();
    }
    g_server.reset();
    
    // 清理 UI
    // 會引起退出crash，不需要
    // obs_queue_task(OBS_TASK_UI, [](void*) {
    //     if (g_status_label) {
    //         QMainWindow* main_window = (QMainWindow*)obs_frontend_get_main_window();
    //         if (main_window && main_window->statusBar()) {
    //             main_window->statusBar()->removeWidget(g_status_label);
    //         }
    //         g_status_label->deleteLater();
    //         g_status_label = nullptr;
    //     }
    // }, nullptr, false);
    
    blog(LOG_INFO, "[gRPC Server] Shutdown complete");
}

}


