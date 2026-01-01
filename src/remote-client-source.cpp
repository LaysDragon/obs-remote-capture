/*
 * remote-client-source.cpp
 * 接收端來源 - 作為 OBS 來源顯示遠端捕捉的視窗
 *
 * 功能:
 * 1. 連接到遠端伺服器
 * 2. 獲取視窗/遊戲列表並顯示在屬性面板
 * 3. 接收並渲染視頻/音頻數據
 */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/threading.h>
#include <util/platform.h>

#include "net-protocol.h"

#ifdef HAVE_GRPC
#include "grpc_client.h"
#endif

#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <vector>
#include <string>
#include <cstring>
#include <sstream>
#include <map>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#define SOCKET_INVALID INVALID_SOCKET
#define CLOSE_SOCKET closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
typedef int socket_t;
#define SOCKET_INVALID (-1)
#define CLOSE_SOCKET close
#endif

// TurboJPEG (可選，用於解碼)
#ifdef HAVE_TURBOJPEG
#include <turbojpeg.h>
#endif

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

    // 網路狀態
    socket_t socket;
    std::atomic<bool> connected;
    std::atomic<bool> streaming;
    std::thread receive_thread;
    std::mutex socket_mutex;  // 保護 socket 操作

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
    std::string cached_properties_json;
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

#ifdef HAVE_TURBOJPEG
    tjhandle jpeg_decompressor;
#endif

#ifdef HAVE_GRPC
    grpc_client_t grpc_client;
#endif

    remote_source_data() :
        source(nullptr),
        server_port(DEFAULT_SERVER_PORT),
        capture_mode(CAPTURE_MODE_WINDOW),
        audio_capture(false),
        socket(SOCKET_INVALID),
        connected(false),
        streaming(false),
        texture(nullptr),
        tex_width(0),
        tex_height(0),
        frame_ready(false)
#ifdef HAVE_TURBOJPEG
        , jpeg_decompressor(nullptr)
#endif
#ifdef HAVE_GRPC
        , grpc_client(nullptr)
#endif
    {}
};

// ========== 輔助函數 ==========
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

// ========== 簡易 JSON 解析輔助函數 ==========
static std::string json_get_string(const std::string& json, const std::string& key, size_t start = 0) {
    std::string search = "\"" + key + "\":\"";
    size_t pos = json.find(search, start);
    if (pos == std::string::npos) return "";
    pos += search.length();
    size_t end = json.find("\"", pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

static int json_get_int(const std::string& json, const std::string& key, size_t start = 0) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search, start);
    if (pos == std::string::npos) return 0;
    pos += search.length();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size()) return 0;
    size_t end = pos;
    if (json[end] == '-') end++;
    while (end < json.size() && isdigit(json[end])) end++;
    if (end == pos) return 0;
    return std::stoi(json.substr(pos, end - pos));
}

static double json_get_double(const std::string& json, const std::string& key, size_t start = 0) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search, start);
    if (pos == std::string::npos) return 0.0;
    pos += search.length();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size()) return 0.0;
    size_t end = pos;
    while (end < json.size() && (isdigit(json[end]) || json[end] == '.' || json[end] == '-' || json[end] == 'e' || json[end] == 'E' || json[end] == '+')) end++;
    if (end == pos) return 0.0;
    return std::stod(json.substr(pos, end - pos));
}

static bool json_get_bool(const std::string& json, const std::string& key, size_t start = 0) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search, start);
    if (pos == std::string::npos) return false;
    pos += search.length();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    return (pos < json.size() && json[pos] == 't');
}

// ========== 解析 JSON 屬性到緩存 ==========
static void parse_properties_json(remote_source_data* data) {
    std::lock_guard<std::mutex> lock(data->props_mutex);
    data->cached_props.clear();
    
    const std::string& json = data->cached_properties_json;
    if (json.empty()) return;
    
    // 找到 properties 數組
    size_t props_start = json.find("\"properties\":[");
    if (props_start == std::string::npos) return;
    props_start += 14;
    
    // 遍歷每個屬性對象
    size_t pos = props_start;
    while (pos < json.size()) {
        size_t obj_start = json.find("{\"name\":", pos);
        if (obj_start == std::string::npos) break;
        
        // 找到這個對象的結束
        int brace = 1;
        size_t obj_end = obj_start + 1;
        while (obj_end < json.size() && brace > 0) {
            if (json[obj_end] == '{') brace++;
            else if (json[obj_end] == '}') brace--;
            obj_end++;
        }
        
        std::string obj = json.substr(obj_start, obj_end - obj_start);
        
        remote_source_data::CachedProperty prop;
        prop.name = json_get_string(obj, "name");
        prop.description = json_get_string(obj, "description");
        prop.type = json_get_int(obj, "type");
        prop.visible = json_get_bool(obj, "visible");
        
        // LIST 類型解析 items
        if (prop.type == OBS_PROPERTY_LIST) {
            size_t items_pos = obj.find("\"items\":[");
            if (items_pos != std::string::npos) {
                items_pos += 9;
                size_t items_end = obj.find("]", items_pos);
                std::string items_str = obj.substr(items_pos, items_end - items_pos);
                
                size_t item_pos = 0;
                while ((item_pos = items_str.find("{\"name\":", item_pos)) != std::string::npos) {
                    std::string item_name = json_get_string(items_str, "name", item_pos);
                    std::string item_value = json_get_string(items_str, "value", item_pos);
                    if (item_value.empty()) item_value = item_name;
                    prop.items.push_back({item_name, item_value});
                    item_pos++;
                }
            }
            prop.current_string = json_get_string(obj, "current");
        }
        
        // BOOL 類型
        if (prop.type == OBS_PROPERTY_BOOL) {
            prop.default_bool = json_get_bool(obj, "default");
        }
        
        // INT 類型
        if (prop.type == OBS_PROPERTY_INT) {
            prop.min_int = json_get_int(obj, "min");
            prop.max_int = json_get_int(obj, "max");
            prop.step_int = json_get_int(obj, "step");
            prop.default_int = json_get_int(obj, "default");
        }
        
        // FLOAT 類型
        if (prop.type == OBS_PROPERTY_FLOAT) {
            prop.min_float = json_get_double(obj, "min");
            prop.max_float = json_get_double(obj, "max");
            prop.step_float = json_get_double(obj, "step");
            prop.default_float = json_get_double(obj, "default");
        }
        
        // TEXT 類型
        if (prop.type == OBS_PROPERTY_TEXT) {
            prop.default_text = json_get_string(obj, "default");
        }
        
        if (!prop.name.empty()) {
            data->cached_props.push_back(prop);
        }
        
        pos = obj_end;
    }
    
    blog(LOG_INFO, "[Remote Source] Parsed %zu properties from JSON", data->cached_props.size());
}

// ========== 連接伺服器 ==========
static bool connect_to_server(remote_source_data* data) {
    if (data->connected.load()) return true;
    if (data->server_ip.empty()) return false;

#ifdef HAVE_GRPC
    // gRPC 模式：創建客戶端並連接
    if (data->grpc_client) {
        grpc_client_destroy(data->grpc_client);
    }
    
    char address[256];
    snprintf(address, sizeof(address), "%s:%d", data->server_ip.c_str(), data->server_port);
    data->grpc_client = grpc_client_create(address);
    
    if (data->grpc_client && grpc_client_wait_connected(data->grpc_client, 5000)) {
        data->connected.store(true);
        blog(LOG_INFO, "[Remote Source] gRPC connected to %s", address);
        return true;
    }
    
    blog(LOG_WARNING, "[Remote Source] gRPC failed to connect to %s", address);
    if (data->grpc_client) {
        grpc_client_destroy(data->grpc_client);
        data->grpc_client = nullptr;
    }
    return false;
#else
    // Socket 模式
#ifdef _WIN32
    static bool wsa_initialized = false;
    if (!wsa_initialized) {
        WSADATA wsa_data;
        WSAStartup(MAKEWORD(2, 2), &wsa_data);
        wsa_initialized = true;
    }
#endif

    // 解析地址
    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", data->server_port);

    if (getaddrinfo(data->server_ip.c_str(), port_str, &hints, &result) != 0) {
        blog(LOG_WARNING, "[Remote Source] Failed to resolve: %s", data->server_ip.c_str());
        return false;
    }

    // 創建並連接 socket
    data->socket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (data->socket == SOCKET_INVALID) {
        freeaddrinfo(result);
        return false;
    }

    if (connect(data->socket, result->ai_addr, (int)result->ai_addrlen) < 0) {
        blog(LOG_WARNING, "[Remote Source] Failed to connect to %s:%d",
             data->server_ip.c_str(), data->server_port);
        CLOSE_SOCKET(data->socket);
        data->socket = SOCKET_INVALID;
        freeaddrinfo(result);
        return false;
    }

    freeaddrinfo(result);
    data->connected.store(true);
    blog(LOG_INFO, "[Remote Source] Connected to %s:%d",
         data->server_ip.c_str(), data->server_port);
    return true;
#endif
}

static void disconnect_from_server(remote_source_data* data) {
    data->streaming.store(false);
    data->connected.store(false);

#ifdef HAVE_GRPC
    if (data->grpc_client) {
        grpc_client_stop_stream(data->grpc_client);
        grpc_client_destroy(data->grpc_client);
        data->grpc_client = nullptr;
    }
#else
    if (data->socket != SOCKET_INVALID) {
        CLOSE_SOCKET(data->socket);
        data->socket = SOCKET_INVALID;
    }

    if (data->receive_thread.joinable()) {
        data->receive_thread.join();
    }
#endif
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
    
    // 斷開現有連接以確保乾淨的 socket 狀態
    if (data->connected.load()) {
        disconnect_from_server(data);
    }
    
    if (!connect_to_server(data)) return false;

#ifdef HAVE_GRPC
    // gRPC 模式：使用 gRPC 客戶端獲取屬性
    const char* source_type = (data->capture_mode == CAPTURE_MODE_GAME) 
        ? "game_capture" : "window_capture";
    
    if (!grpc_client_get_properties(data->grpc_client, source_type)) {
        blog(LOG_WARNING, "[Remote Source] gRPC GetProperties failed");
        return false;
    }
    
    // 從 gRPC 響應填充 cached_props
    {
        std::lock_guard<std::mutex> lock(data->props_mutex);
        data->cached_props.clear();
        data->window_list.clear();
        
        size_t count = grpc_client_property_count(data->grpc_client);
        for (size_t i = 0; i < count; i++) {
            remote_source_data::CachedProperty prop;
            prop.name = grpc_client_property_name(data->grpc_client, i);
            prop.description = grpc_client_property_description(data->grpc_client, i);
            prop.type = grpc_client_property_type(data->grpc_client, i);
            prop.visible = true;
            
            // 處理 LIST 類型
            if (prop.type == 6) {  // OBS_PROPERTY_LIST
                size_t item_count = grpc_client_property_item_count(data->grpc_client, i);
                for (size_t j = 0; j < item_count; j++) {
                    const char* item_name = grpc_client_property_item_name(data->grpc_client, i, j);
                    const char* item_value = grpc_client_property_item_value(data->grpc_client, i, j);
                    prop.items.push_back({
                        item_name ? item_name : "",
                        item_value ? item_value : ""
                    });
                    
                    // 填充視窗列表 (如果是 window 屬性)
                    if (prop.name == "window") {
                        data->window_list.push_back({
                            item_name ? item_name : "",
                            item_value ? item_value : ""
                        });
                    }
                }
            }
            
            data->cached_props.push_back(prop);
        }
    }
    
    blog(LOG_INFO, "[Remote Source] gRPC: Got %zu properties, %zu windows",
         data->cached_props.size(), data->window_list.size());
    return true;
#else
    // Socket 模式
    // 使用 socket mutex 保護操作
    std::lock_guard<std::mutex> socket_lock(data->socket_mutex);

    // 發送請求
    PacketHeader header;
    GetPropertiesRequest req;
    req.capture_mode = (uint8_t)data->capture_mode;

    packet_header_init(&header, MSG_GET_PROPERTIES, sizeof(req));
    if (!send_all(data->socket, &header, sizeof(header))) {
        blog(LOG_WARNING, "[Remote Source] Failed to send properties request header");
        return false;
    }
    if (!send_all(data->socket, &req, sizeof(req))) {
        blog(LOG_WARNING, "[Remote Source] Failed to send properties request body");
        return false;
    }

    // 接收回應
    if (!recv_all(data->socket, &header, sizeof(header))) {
        blog(LOG_WARNING, "[Remote Source] Failed to receive properties response header");
        return false;
    }
    if (!packet_header_validate(&header)) {
        blog(LOG_WARNING, "[Remote Source] Invalid properties response header (magic=0x%08X, version=%d)", 
             header.magic, header.version);
        return false;
    }
    if (header.type != MSG_PROPERTIES_RESPONSE) {
        blog(LOG_WARNING, "[Remote Source] Unexpected response type: %d (expected %d)", 
             header.type, MSG_PROPERTIES_RESPONSE);
        return false;
    }

    std::vector<char> json_data(header.payload_size + 1);
    if (!recv_all(data->socket, json_data.data(), header.payload_size)) {
        blog(LOG_WARNING, "[Remote Source] Failed to receive properties JSON data");
        return false;
    }
    json_data[header.payload_size] = '\0';

    // 存儲 JSON
    {
        std::lock_guard<std::mutex> lock(data->props_mutex);
        data->cached_properties_json = json_data.data();
    }

    // 輸出原始 JSON 以便調試
    blog(LOG_INFO, "[Remote Source] Received JSON (length=%zu): %.500s...", 
         data->cached_properties_json.size(), data->cached_properties_json.c_str());

    // 解析視窗列表
    data->window_list.clear();

    std::string json_str = data->cached_properties_json;
    
    // 輔助函數: 從指定位置找到屬性的 items 並解析
    auto parse_items_from_pos = [&](size_t search_start) -> bool {
        size_t items_pos = json_str.find("\"items\":[", search_start);
        if (items_pos == std::string::npos) return false;
        
        // 確保這個 items 屬於正確的屬性（在下一個屬性之前）
        size_t next_prop = json_str.find("},{\"name\":", search_start + 1);
        if (next_prop != std::string::npos && items_pos > next_prop) return false;
        
        // 找到 items 的結束位置 (匹配括號)
        size_t start = items_pos + 9;
        int bracket_count = 1;
        size_t end = start;
        while (end < json_str.size() && bracket_count > 0) {
            if (json_str[end] == '[') bracket_count++;
            else if (json_str[end] == ']') bracket_count--;
            end++;
        }
        if (bracket_count != 0) return false;
        
        end--;  // 不包括最後的 ]
        std::string items_str = json_str.substr(start, end - start);
        
        // 解析每個 item
        size_t pos = 0;
        while ((pos = items_str.find("{\"name\":\"", pos)) != std::string::npos) {
            pos += 9;  // 跳過 {"name":"
            size_t name_end = items_str.find("\"", pos);
            if (name_end == std::string::npos) break;
            std::string name = items_str.substr(pos, name_end - pos);

            // 尋找 value
            size_t obj_end = items_str.find("}", name_end);
            size_t value_pos = items_str.find("\"value\":\"", name_end);
            std::string value;
            if (value_pos != std::string::npos && value_pos < obj_end) {
                value_pos += 9;
                size_t value_end = items_str.find("\"", value_pos);
                if (value_end != std::string::npos && value_end < obj_end) {
                    value = items_str.substr(value_pos, value_end - value_pos);
                }
            }

            if (!name.empty()) {
                data->window_list.push_back({name, value.empty() ? name : value});
            }
            pos = obj_end != std::string::npos ? obj_end : name_end;
        }
        return !data->window_list.empty();
    };
    
    // 針對 window_capture 和 game_capture 嘗試不同的屬性名稱
    // window_capture 使用 "window" 屬性
    // game_capture 也使用 "window" 屬性 (當 mode 設為 CAPTURE_MODE_WINDOW 時)
    size_t window_prop_pos = json_str.find("\"name\":\"window\"");
    if (window_prop_pos != std::string::npos) {
        parse_items_from_pos(window_prop_pos);
    }
    
    // 如果沒有找到視窗列表，嘗試找 game_capture 的 mode 屬性
    if (data->window_list.empty()) {
        size_t mode_prop_pos = json_str.find("\"name\":\"mode\"");
        if (mode_prop_pos != std::string::npos) {
            // Game capture mode: 添加模式選項作為臨時列表
            parse_items_from_pos(mode_prop_pos);
            blog(LOG_INFO, "[Remote Source] Found game capture mode options: %zu items", 
                 data->window_list.size());
        }
    }

    blog(LOG_INFO, "[Remote Source] Parsed %zu items from properties",
         data->window_list.size());
    
    // 解析所有屬性到緩存用於動態 UI
    parse_properties_json(data);
    
    return true;
#endif  // HAVE_GRPC
}

// ========== 接收線程 ==========
static void receive_thread_func(remote_source_data* data) {
    blog(LOG_INFO, "[Remote Source] Receive thread started");

    while (data->streaming.load() && data->connected.load()) {
        PacketHeader header;
        if (!recv_all(data->socket, &header, sizeof(header))) {
            blog(LOG_WARNING, "[Remote Source] Connection lost");
            break;
        }

        if (!packet_header_validate(&header)) {
            blog(LOG_WARNING, "[Remote Source] Invalid packet");
            continue;
        }

        switch (header.type) {
        case MSG_VIDEO_FRAME: {
            // 讀取視頻頭
            VideoFrameHeader video_header;
            if (!recv_all(data->socket, &video_header, sizeof(video_header))) break;

            size_t jpeg_size = header.payload_size - sizeof(video_header);
            std::vector<uint8_t> jpeg_data(jpeg_size);
            if (!recv_all(data->socket, jpeg_data.data(), jpeg_size)) break;

#ifdef HAVE_TURBOJPEG
            // 解碼 JPEG
            int width, height, subsamp, colorspace;
            if (tjDecompressHeader3(data->jpeg_decompressor,
                    jpeg_data.data(), (unsigned long)jpeg_size,
                    &width, &height, &subsamp, &colorspace) == 0) {

                // 分配緩衝區
                size_t buffer_size = width * height * 4;  // RGBA
                std::vector<uint8_t> decoded(buffer_size);

                if (tjDecompress2(data->jpeg_decompressor,
                        jpeg_data.data(), (unsigned long)jpeg_size,
                        decoded.data(), width, 0, height,
                        TJPF_BGRA, TJFLAG_FASTDCT) == 0) {

                    // 更新紋理緩衝
                    std::lock_guard<std::mutex> lock(data->video_mutex);
                    data->tex_width = width;
                    data->tex_height = height;
                    data->frame_buffer = std::move(decoded);
                    data->frame_ready = true;
                }
            }
#endif
            break;
        }

        case MSG_AUDIO_FRAME: {
            // 讀取音頻頭
            AudioFrameHeader audio_header;
            if (!recv_all(data->socket, &audio_header, sizeof(audio_header))) break;

            size_t audio_size = header.payload_size - sizeof(audio_header);
            std::vector<float> audio_data(audio_size / sizeof(float));
            if (!recv_all(data->socket, audio_data.data(), audio_size)) break;

            // 加入音頻隊列
            std::lock_guard<std::mutex> lock(data->audio_mutex);
            data->audio_queue.push(std::move(audio_data));

            // 限制隊列大小
            while (data->audio_queue.size() > 10) {
                data->audio_queue.pop();
            }
            break;
        }

        default:
            // 跳過未知類型
            if (header.payload_size > 0) {
                std::vector<char> skip(header.payload_size);
                recv_all(data->socket, skip.data(), header.payload_size);
            }
            break;
        }
    }

    data->streaming.store(false);
    blog(LOG_INFO, "[Remote Source] Receive thread stopped");
}

// ========== 開始串流 ==========
#ifdef HAVE_GRPC
// gRPC 視頻回調
static void grpc_video_callback(uint32_t width, uint32_t height,
                                 const uint8_t* jpeg_data, size_t jpeg_size,
                                 uint64_t timestamp_ns, void* user_data) {
    UNUSED_PARAMETER(timestamp_ns);
    remote_source_data* data = (remote_source_data*)user_data;
    if (!data) return;
    
#ifdef HAVE_TURBOJPEG
    // 解碼 JPEG
    int w, h, subsamp, colorspace;
    if (tjDecompressHeader3(data->jpeg_decompressor,
            jpeg_data, (unsigned long)jpeg_size,
            &w, &h, &subsamp, &colorspace) == 0) {
        
        size_t buffer_size = w * h * 4;  // RGBA
        std::vector<uint8_t> decoded(buffer_size);
        
        if (tjDecompress2(data->jpeg_decompressor,
                jpeg_data, (unsigned long)jpeg_size,
                decoded.data(), w, 0, h,
                TJPF_BGRA, TJFLAG_FASTDCT) == 0) {
            
            std::lock_guard<std::mutex> lock(data->video_mutex);
            data->tex_width = w;
            data->tex_height = h;
            data->frame_buffer = std::move(decoded);
            data->frame_ready = true;
        }
    }
#else
    UNUSED_PARAMETER(width);
    UNUSED_PARAMETER(height);
    UNUSED_PARAMETER(jpeg_data);
    UNUSED_PARAMETER(jpeg_size);
#endif
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
#endif  // HAVE_GRPC

static void start_streaming(remote_source_data* data) {
    if (data->streaming.load()) {
        blog(LOG_DEBUG, "[Remote Source] Already streaming, ignoring start request");
        return;
    }
    if (!connect_to_server(data)) return;

    blog(LOG_INFO, "[Remote Source] Starting stream with window: %s", data->selected_window.c_str());

#ifdef HAVE_GRPC
    // gRPC 模式：使用 gRPC 客戶端開始串流
    const char* source_type = (data->capture_mode == CAPTURE_MODE_GAME) 
        ? "game_capture" : "window_capture";
    
    // 構建設定
    const char* keys[] = {"window"};
    const char* values[] = {data->selected_window.c_str()};
    
    data->streaming.store(true);
    
    if (!grpc_client_start_stream(data->grpc_client, source_type,
            keys, values, 1,
            grpc_video_callback, grpc_audio_callback, data)) {
        blog(LOG_WARNING, "[Remote Source] gRPC StartStream failed");
        data->streaming.store(false);
    } else {
        blog(LOG_INFO, "[Remote Source] gRPC stream started");
    }
#else
    // Socket 模式
    // 構建設定 JSON
    std::ostringstream json;
    json << "{";
    json << "\"__capture_mode__\":" << data->capture_mode << ",";

    // 視窗/遊戲 ID
    if (data->capture_mode == CAPTURE_MODE_WINDOW) {
        json << "\"window\":\"" << data->selected_window << "\",";
    } else {
        json << "\"capture_any_fullscreen\":false,";
        json << "\"window\":\"" << data->selected_window << "\",";
    }

    // 音頻捕捉
    if (data->capture_mode == CAPTURE_MODE_GAME) {
        json << "\"capture_audio\":" << (data->audio_capture ? "true" : "false") << ",";
    }

    // 移除最後的逗號
    std::string json_str = json.str();
    if (json_str.back() == ',') {
        json_str.pop_back();
    }
    json_str += "}";

    // 發送開始串流請求 (需要 socket 保護)
    {
        std::lock_guard<std::mutex> lock(data->socket_mutex);
        PacketHeader header;
        packet_header_init(&header, MSG_START_STREAM, (uint32_t)json_str.size());
        send_all(data->socket, &header, sizeof(header));
        send_all(data->socket, json_str.data(), json_str.size());
    }

    // 啟動接收線程
    data->streaming.store(true);
    data->receive_thread = std::thread(receive_thread_func, data);
#endif  // HAVE_GRPC
}

static void stop_streaming(remote_source_data* data) {
    if (!data->streaming.load()) return;

    blog(LOG_INFO, "[Remote Source] Stopping stream");

    // 先標記停止，讓接收線程知道要結束
    data->streaming.store(false);

#ifdef HAVE_GRPC
    // gRPC 模式
    if (data->grpc_client) {
        grpc_client_stop_stream(data->grpc_client);
    }
#else
    // Socket 模式
    // 發送停止請求 (需要 socket 保護)
    if (data->connected.load() && data->socket != SOCKET_INVALID) {
        std::lock_guard<std::mutex> lock(data->socket_mutex);
        PacketHeader header;
        packet_header_init(&header, MSG_STOP_STREAM, 0);
        send_all(data->socket, &header, sizeof(header));
    }

    // 等待接收線程結束
    if (data->receive_thread.joinable()) {
        data->receive_thread.join();
    }
#endif  // HAVE_GRPC

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

#ifdef HAVE_TURBOJPEG
    data->jpeg_decompressor = tjInitDecompress();
#endif

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

#ifdef HAVE_TURBOJPEG
    if (data->jpeg_decompressor) {
        tjDestroy(data->jpeg_decompressor);
    }
#endif

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


