/*
 * plugin-main.c
 * OBS Remote Window Capture 插件入口點
 *
 * 此插件包含兩個組件:
 * 1. Remote Server: 在發送端運行，提供視窗列表並串流畫面
 * 2. Remote Source: 在接收端運行，作為 OBS 來源顯示遠端畫面
 */

#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-remote-window-capture", "en-US")

// 外部聲明 (定義於其他源文件)
extern struct obs_source_info remote_source_info;
extern struct obs_source_info capture_preview_info;

// 初始化函數
extern void init_remote_source_info(void);
extern void init_capture_preview_info(void);

// 伺服器啟動/停止函數
#ifdef HAVE_GRPC
extern void obs_grpc_server_start(void);
extern void obs_grpc_server_stop(void);
#else
extern void remote_server_start(void);
extern void remote_server_stop(void);
#endif

// 插件載入時調用
bool obs_module_load(void)
{
    blog(LOG_INFO, "[Remote Window Capture] Loading plugin...");

    // 初始化來源信息結構
    init_remote_source_info();
    init_capture_preview_info();

    // 註冊遠端來源 (接收端使用)
    obs_register_source(&remote_source_info);
    
    // 註冊本地預覽來源 (驗證用)
    obs_register_source(&capture_preview_info);

    // 啟動遠端伺服器 (發送端使用)
#ifdef HAVE_GRPC
    obs_grpc_server_start();
    blog(LOG_INFO, "[Remote Window Capture] gRPC server started on port 44555");
#else
    remote_server_start();
#endif

    blog(LOG_INFO, "[Remote Window Capture] Plugin loaded successfully.");
    return true;
}

// 插件卸載時調用
void obs_module_unload(void)
{
    blog(LOG_INFO, "[Remote Window Capture] Unloading plugin...");

    // 停止遠端伺服器
#ifdef HAVE_GRPC
    obs_grpc_server_stop();
#else
    remote_server_stop();
#endif

    blog(LOG_INFO, "[Remote Window Capture] Plugin unloaded.");
}

// 插件描述
const char *obs_module_name(void)
{
    return "Remote Window Capture";
}

const char *obs_module_description(void)
{
    return "Capture windows from a remote computer running OBS with this plugin.";
}

