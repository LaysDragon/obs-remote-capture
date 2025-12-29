/*
 * remote-server.cpp
 * 發送端伺服器邏輯
 *
 * 功能:
 * 1. 監聽 TCP 連接
 * 2. 接收 GET_PROPERTIES 請求，返回視窗/遊戲列表 (JSON)
 * 3. 接收 START_STREAM 請求，創建捕捉源並串流影音
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/threading.h>
#include <util/platform.h>
#include <graphics/graphics.h>

#include "net-protocol.h"

#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <cstring>
#include <sstream>
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#define SOCKET_INVALID INVALID_SOCKET
#define SOCKET_ERROR_CODE WSAGetLastError()
#define CLOSE_SOCKET closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int socket_t;
#define SOCKET_INVALID (-1)
#define SOCKET_ERROR_CODE errno
#define CLOSE_SOCKET close
#endif

// TurboJPEG (可選，用於影像壓縮)
#ifdef HAVE_TURBOJPEG
#include <turbojpeg.h>
#endif

// ========== 全局狀態 ==========
static std::atomic<bool> g_server_running{false};
static std::thread g_server_thread;
static socket_t g_server_socket = SOCKET_INVALID;

// 當前活躍的串流會話
struct StreamSession {
    socket_t client_socket;
    obs_source_t* capture_source;
    std::atomic<bool> active;
    std::thread stream_thread;
    std::thread* client_thread;  // 指向外部的客戶端線程
    
    // 渲染相關
    gs_texrender_t* texrender;
    gs_stagesurf_t* stagesurface;
    uint32_t width;
    uint32_t height;
    
#ifdef HAVE_TURBOJPEG
    tjhandle jpeg_compressor;
#endif
    uint32_t frame_number;

    StreamSession() : 
        client_socket(SOCKET_INVALID), 
        capture_source(nullptr), 
        active(false),
        client_thread(nullptr),
        texrender(nullptr),
        stagesurface(nullptr),
        width(0),
        height(0),
#ifdef HAVE_TURBOJPEG
        jpeg_compressor(nullptr),
#endif
        frame_number(0)
    {}
};

static std::mutex g_sessions_mutex;
static std::vector<std::unique_ptr<StreamSession>> g_sessions;
static std::mutex g_client_threads_mutex;
static std::vector<std::thread> g_client_threads;

// ========== 輔助函數: 發送數據 ==========
static bool send_all(socket_t sock, const void* data, size_t len) {
    const char* ptr = (const char*)data;
    size_t remaining = len;
    while (remaining > 0) {
        int sent = send(sock, ptr, (int)remaining, 0);
        if (sent <= 0) return false;
        ptr += sent;
        remaining -= sent;
    }
    return true;
}

// ========== 輔助函數: 接收數據 ==========
static bool recv_all(socket_t sock, void* data, size_t len) {
    char* ptr = (char*)data;
    size_t remaining = len;
    while (remaining > 0) {
        int received = recv(sock, ptr, (int)remaining, 0);
        if (received <= 0) return false;
        ptr += received;
        remaining -= received;
    }
    return true;
}

// ========== 獲取當前時間戳 (毫秒) ==========
static uint32_t get_timestamp_ms() {
    return (uint32_t)(os_gettime_ns() / 1000000ULL);
}

// ========== 轉義 JSON 字符串 ==========
static void json_escape_string(std::ostringstream& json, const char* str) {
    if (!str) return;
    for (const char* p = str; *p; p++) {
        if (*p == '\"') json << "\\\"";
        else if (*p == '\\') json << "\\\\";
        else if (*p == '\n') json << "\\n";
        else if (*p == '\r') json << "\\r";
        else if (*p == '\t') json << "\\t";
        else if ((unsigned char)*p < 32) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)*p);
            json << buf;
        }
        else json << *p;
    }
}

// ========== 序列化單個屬性為 JSON ==========
static void serialize_property(std::ostringstream& json, obs_property_t* prop, 
                                obs_data_t* settings, bool* first_prop) {
    const char* name = obs_property_name(prop);
    const char* desc = obs_property_description(prop);
    obs_property_type type = obs_property_get_type(prop);
    
    // 跳過不可見的屬性
    if (!obs_property_visible(prop)) return;

    if (!*first_prop) json << ",";
    *first_prop = false;

    json << "{";
    json << "\"name\":\"" << (name ? name : "") << "\",";
    json << "\"description\":\"";
    json_escape_string(json, desc);
    json << "\",";
    json << "\"type\":" << (int)type;

    // 根據類型序列化
    switch (type) {
    case OBS_PROPERTY_LIST: {
        json << ",\"format\":" << (int)obs_property_list_format(prop);
        json << ",\"items\":[";
        size_t count = obs_property_list_item_count(prop);
        for (size_t i = 0; i < count; i++) {
            if (i > 0) json << ",";
            const char* item_name = obs_property_list_item_name(prop, i);
            obs_combo_format format = obs_property_list_format(prop);
            
            json << "{\"name\":\"";
            json_escape_string(json, item_name);
            json << "\"";

            if (format == OBS_COMBO_FORMAT_STRING) {
                const char* item_val = obs_property_list_item_string(prop, i);
                json << ",\"value\":\"";
                json_escape_string(json, item_val);
                json << "\"";
            } else if (format == OBS_COMBO_FORMAT_INT) {
                long long item_val = obs_property_list_item_int(prop, i);
                json << ",\"value\":" << item_val;
            }
            json << "}";
        }
        json << "]";
        
        // 當前選中的值
        if (settings && name) {
            obs_combo_format format = obs_property_list_format(prop);
            if (format == OBS_COMBO_FORMAT_STRING) {
                const char* val = obs_data_get_string(settings, name);
                json << ",\"current\":\"";
                json_escape_string(json, val);
                json << "\"";
            } else if (format == OBS_COMBO_FORMAT_INT) {
                long long val = obs_data_get_int(settings, name);
                json << ",\"current\":" << val;
            }
        }
        break;
    }
    case OBS_PROPERTY_BOOL: {
        bool val = settings ? obs_data_get_bool(settings, name) : false;
        json << ",\"default\":" << (val ? "true" : "false");
        break;
    }
    case OBS_PROPERTY_INT: {
        int min_val = (int)obs_property_int_min(prop);
        int max_val = (int)obs_property_int_max(prop);
        int step = (int)obs_property_int_step(prop);
        int val = settings ? (int)obs_data_get_int(settings, name) : min_val;
        json << ",\"min\":" << min_val << ",\"max\":" << max_val 
             << ",\"step\":" << step << ",\"default\":" << val;
        break;
    }
    case OBS_PROPERTY_FLOAT: {
        double min_val = obs_property_float_min(prop);
        double max_val = obs_property_float_max(prop);
        double step = obs_property_float_step(prop);
        double val = settings ? obs_data_get_double(settings, name) : min_val;
        json << ",\"min\":" << min_val << ",\"max\":" << max_val 
             << ",\"step\":" << step << ",\"default\":" << val;
        break;
    }
    case OBS_PROPERTY_TEXT: {
        const char* val = settings ? obs_data_get_string(settings, name) : "";
        json << ",\"default\":\"";
        json_escape_string(json, val);
        json << "\"";
        break;
    }
    default:
        break;
    }

    json << "}";
}

// ========== 序列化屬性為 JSON ==========
static std::string serialize_properties_to_json(const char* source_type) {
    std::ostringstream json;
    json << "{";
    json << "\"source_type\":\"" << source_type << "\",";
    json << "\"properties\":[";

    // 創建臨時來源以獲取屬性
    // 注意：需要在主線程上執行
    obs_source_t* temp_source = obs_source_create(source_type, "__temp_enum__", nullptr, nullptr);
    if (!temp_source) {
        blog(LOG_WARNING, "[Remote Server] Failed to create temp source: %s", source_type);
        json << "]}";
        return json.str();
    }

    // 獲取屬性
    obs_properties_t* props = obs_source_properties(temp_source);
    if (!props) {
        obs_source_release(temp_source);
        json << "]}";
        return json.str();
    }

    // 獲取當前設定
    obs_data_t* settings = obs_source_get_settings(temp_source);

    bool first_prop = true;
    obs_property_t* prop = obs_properties_first(props);

    while (prop) {
        serialize_property(json, prop, settings, &first_prop);
        obs_property_next(&prop);
    }

    obs_data_release(settings);
    obs_properties_destroy(props);
    obs_source_release(temp_source);

    json << "]}";
    
    blog(LOG_DEBUG, "[Remote Server] Serialized properties for %s", source_type);
    return json.str();
}

// ========== 捕獲並發送一幀 ==========
static void capture_and_send_frame(StreamSession* session) {
    if (!session || !session->capture_source || !session->active.load()) return;
    
    uint32_t width = obs_source_get_width(session->capture_source);
    uint32_t height = obs_source_get_height(session->capture_source);
    
    if (width == 0 || height == 0) return;
    
    // 如果尺寸變化，重新創建渲染資源
    if (width != session->width || height != session->height) {
        obs_enter_graphics();
        
        if (session->texrender) {
            gs_texrender_destroy(session->texrender);
        }
        if (session->stagesurface) {
            gs_stagesurface_destroy(session->stagesurface);
        }
        
        session->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
        session->stagesurface = gs_stagesurface_create(width, height, GS_BGRA);
        session->width = width;
        session->height = height;
        
        obs_leave_graphics();
    }
    
    if (!session->texrender || !session->stagesurface) return;
    
    obs_enter_graphics();
    
    // 渲染源到紋理
    if (gs_texrender_begin(session->texrender, width, height)) {
        struct vec4 clear_color;
        vec4_zero(&clear_color);
        gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
        gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
        
        obs_source_video_render(session->capture_source);
        
        gs_texrender_end(session->texrender);
    }
    
    // 從紋理複製到 staging surface
    gs_texture_t* tex = gs_texrender_get_texture(session->texrender);
    if (tex) {
        gs_stage_texture(session->stagesurface, tex);
        
        // 讀取像素數據
        uint8_t* data;
        uint32_t linesize;
        if (gs_stagesurface_map(session->stagesurface, &data, &linesize)) {
#ifdef HAVE_TURBOJPEG
            // 壓縮為 JPEG
            unsigned long jpeg_size = 0;
            unsigned char* jpeg_buf = nullptr;
            
            int result = tjCompress2(
                session->jpeg_compressor,
                data,
                width, linesize,
                height,
                TJPF_BGRA,
                &jpeg_buf,
                &jpeg_size,
                TJSAMP_420,
                85,  // 品質
                TJFLAG_FASTDCT
            );
            
            if (result == 0 && jpeg_buf) {
                // 發送封包
                PacketHeader header;
                packet_header_init(&header, MSG_VIDEO_FRAME, sizeof(VideoFrameHeader) + (uint32_t)jpeg_size);
                header.timestamp_ms = get_timestamp_ms();
                
                VideoFrameHeader video_header;
                video_header.width = width;
                video_header.height = height;
                video_header.format = 1;  // JPEG
                video_header.frame_number = session->frame_number++;
                
                send_all(session->client_socket, &header, sizeof(header));
                send_all(session->client_socket, &video_header, sizeof(video_header));
                send_all(session->client_socket, jpeg_buf, jpeg_size);
                
                tjFree(jpeg_buf);
            }
#else
            // 沒有 TurboJPEG，跳過
            UNUSED_PARAMETER(data);
            UNUSED_PARAMETER(linesize);
#endif
            gs_stagesurface_unmap(session->stagesurface);
        }
    }
    
    obs_leave_graphics();
}

// ========== 串流線程 ==========
static void stream_thread_func(StreamSession* session) {
    blog(LOG_INFO, "[Remote Server] Stream thread started");
    
    while (session->active.load() && g_server_running.load()) {
        capture_and_send_frame(session);
        
        // 約 30 FPS
        os_sleep_ms(33);
    }
    
    blog(LOG_INFO, "[Remote Server] Stream thread stopped");
}

// ========== 處理客戶端連接 ==========
static void handle_client(socket_t client_socket) {
    blog(LOG_INFO, "[Remote Server] Client connected");

    auto session = std::make_unique<StreamSession>();
    session->client_socket = client_socket;
    session->active.store(true);
    
#ifdef HAVE_TURBOJPEG
    session->jpeg_compressor = tjInitCompress();
#endif

    while (session->active.load() && g_server_running.load()) {
        PacketHeader header;
        if (!recv_all(client_socket, &header, sizeof(header))) {
            break;  // 連接斷開
        }

        if (!packet_header_validate(&header)) {
            blog(LOG_WARNING, "[Remote Server] Invalid packet header");
            continue;
        }

        // 讀取有效載荷
        std::vector<char> payload(header.payload_size);
        if (header.payload_size > 0) {
            if (!recv_all(client_socket, payload.data(), header.payload_size)) {
                break;
            }
        }

        switch (header.type) {
        case MSG_GET_PROPERTIES: {
            // 解析請求
            if (payload.size() >= sizeof(GetPropertiesRequest)) {
                GetPropertiesRequest* req = (GetPropertiesRequest*)payload.data();
                const char* source_type = (req->capture_mode == CAPTURE_MODE_GAME)
                    ? "game_capture" : "window_capture";

                std::string json = serialize_properties_to_json(source_type);

                // 發送回應
                PacketHeader resp_header;
                packet_header_init(&resp_header, MSG_PROPERTIES_RESPONSE, (uint32_t)json.size());
                send_all(client_socket, &resp_header, sizeof(resp_header));
                send_all(client_socket, json.data(), json.size());
            }
            break;
        }

        case MSG_START_STREAM: {
            // 停止之前的串流
            if (session->stream_thread.joinable()) {
                session->active.store(false);
                session->stream_thread.join();
                session->active.store(true);
            }
            
            // 釋放之前的捕捉源
            if (session->capture_source) {
                obs_source_release(session->capture_source);
                session->capture_source = nullptr;
            }

            // 解析 JSON 設定
            payload.push_back('\0');
            obs_data_t* settings = obs_data_create_from_json(payload.data());

            if (settings) {
                // 獲取捕捉模式
                int mode = (int)obs_data_get_int(settings, "__capture_mode__");
                const char* source_type = (mode == CAPTURE_MODE_GAME)
                    ? "game_capture" : "window_capture";

                // 創建捕捉源
                session->capture_source = obs_source_create_private(
                    source_type, "__remote_capture__", settings);

                if (session->capture_source) {
                    blog(LOG_INFO, "[Remote Server] Started streaming: %s", source_type);
                    
                    // 啟動串流線程
                    session->stream_thread = std::thread(stream_thread_func, session.get());
                }

                obs_data_release(settings);
            }
            break;
        }

        case MSG_STOP_STREAM: {
            // 停止串流線程
            if (session->stream_thread.joinable()) {
                session->active.store(false);
                session->stream_thread.join();
                session->active.store(true);
            }
            
            if (session->capture_source) {
                obs_source_release(session->capture_source);
                session->capture_source = nullptr;
                blog(LOG_INFO, "[Remote Server] Stopped streaming");
            }
            break;
        }

        case MSG_HEARTBEAT:
            // 回應心跳
            send_all(client_socket, &header, sizeof(header));
            break;

        default:
            blog(LOG_WARNING, "[Remote Server] Unknown message type: %d", header.type);
            break;
        }
    }

    // 清理
    session->active.store(false);
    
    if (session->stream_thread.joinable()) {
        session->stream_thread.join();
    }
    
    obs_enter_graphics();
    if (session->texrender) {
        gs_texrender_destroy(session->texrender);
    }
    if (session->stagesurface) {
        gs_stagesurface_destroy(session->stagesurface);
    }
    obs_leave_graphics();
    
    if (session->capture_source) {
        obs_source_release(session->capture_source);
    }

#ifdef HAVE_TURBOJPEG
    if (session->jpeg_compressor) {
        tjDestroy(session->jpeg_compressor);
    }
#endif

    CLOSE_SOCKET(client_socket);
    blog(LOG_INFO, "[Remote Server] Client disconnected");
}

// ========== 伺服器主循環 ==========
static void server_thread_func() {
    blog(LOG_INFO, "[Remote Server] Server thread started on port %d", DEFAULT_SERVER_PORT);

#ifdef _WIN32
    // 初始化 Winsock
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        blog(LOG_ERROR, "[Remote Server] WSAStartup failed");
        return;
    }
#endif

    // 創建 socket
    g_server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_server_socket == SOCKET_INVALID) {
        blog(LOG_ERROR, "[Remote Server] Failed to create socket");
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    // 設置 SO_REUSEADDR
    int opt = 1;
    setsockopt(g_server_socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    // 綁定地址
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(DEFAULT_SERVER_PORT);

    if (bind(g_server_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        blog(LOG_ERROR, "[Remote Server] Failed to bind to port %d", DEFAULT_SERVER_PORT);
        CLOSE_SOCKET(g_server_socket);
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    // 開始監聽
    if (listen(g_server_socket, 5) < 0) {
        blog(LOG_ERROR, "[Remote Server] Failed to listen");
        CLOSE_SOCKET(g_server_socket);
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    blog(LOG_INFO, "[Remote Server] Listening for connections...");

    while (g_server_running.load()) {
        // 設置超時以便可以檢查 g_server_running
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(g_server_socket, &read_fds);

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int result = select((int)g_server_socket + 1, &read_fds, nullptr, nullptr, &timeout);
        if (result <= 0) continue;

        // 接受連接
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        socket_t client_socket = accept(g_server_socket, (struct sockaddr*)&client_addr, &client_len);

        if (client_socket == SOCKET_INVALID) continue;

        // 在新線程中處理客戶端 (使用可追蹤的方式)
        {
            std::lock_guard<std::mutex> lock(g_client_threads_mutex);
            // 清理已完成的線程
            g_client_threads.erase(
                std::remove_if(g_client_threads.begin(), g_client_threads.end(),
                    [](std::thread& t) { 
                        if (t.joinable()) {
                            // 嘗試非阻塞檢查 (這裡簡單地假設線程仍在運行)
                            return false;
                        }
                        return true;
                    }),
                g_client_threads.end()
            );
            // 添加新線程
            g_client_threads.emplace_back(handle_client, client_socket);
        }
    }

    blog(LOG_INFO, "[Remote Server] Shutting down server...");

    // 關閉所有客戶端連接以使線程結束
    {
        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        for (auto& session : g_sessions) {
            session->active.store(false);
            if (session->client_socket != SOCKET_INVALID) {
                CLOSE_SOCKET(session->client_socket);
            }
        }
    }

    // 等待所有客戶端線程結束
    {
        std::lock_guard<std::mutex> lock(g_client_threads_mutex);
        for (auto& t : g_client_threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        g_client_threads.clear();
    }

    CLOSE_SOCKET(g_server_socket);
    g_server_socket = SOCKET_INVALID;

#ifdef _WIN32
    WSACleanup();
#endif

    blog(LOG_INFO, "[Remote Server] Server thread stopped");
}

// ========== 公開 API ==========
extern "C" {

void remote_server_start(void) {
    if (g_server_running.load()) return;

    g_server_running.store(true);
    g_server_thread = std::thread(server_thread_func);
}

void remote_server_stop(void) {
    if (!g_server_running.load()) return;

    blog(LOG_INFO, "[Remote Server] Stopping server...");
    g_server_running.store(false);

    // 關閉監聽 socket 以中斷 accept
    if (g_server_socket != SOCKET_INVALID) {
        CLOSE_SOCKET(g_server_socket);
        g_server_socket = SOCKET_INVALID;
    }

    // 關閉所有客戶端連接
    {
        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        for (auto& session : g_sessions) {
            session->active.store(false);
            if (session->client_socket != SOCKET_INVALID) {
                CLOSE_SOCKET(session->client_socket);
            }
        }
    }

    // 等待服務器線程結束
    if (g_server_thread.joinable()) {
        g_server_thread.join();
    }

    blog(LOG_INFO, "[Remote Server] Server stopped");
}

}  // extern "C"

