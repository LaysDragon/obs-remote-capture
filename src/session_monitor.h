/*
 * session_monitor.h
 * Session 監控器停駐窗口
 */

#pragma once

#include <QDockWidget>
#include <QTimer>
#include <QLabel>
#include <QVBoxLayout>
#include <QTableWidget>
#include <QString>
#include <vector>

// Session 狀態資訊
struct SessionStatus {
    std::string id;
    std::string source_type;
    std::string encoder_name;
    bool streaming;
    uint64_t stream_id;       // FlowMeter stream ID
    uint32_t width;
    uint32_t height;
    uint32_t frame_count;
};

// 獲取所有 session 狀態的函數
extern std::vector<SessionStatus> grpc_server_get_session_status();

class SessionMonitorDock : public QDockWidget {
    Q_OBJECT
    
public:
    explicit SessionMonitorDock(QWidget* parent = nullptr);
    ~SessionMonitorDock();
    
private slots:
    void updateStatus();
    
private:
    QTimer* timer_;
    QTableWidget* table_;
    QLabel* summary_label_;
};

// 創建並註冊停駐窗口
#ifdef __cplusplus
extern "C" {
#endif
void session_monitor_init(void);
void session_monitor_cleanup(void);
#ifdef __cplusplus
}
#endif

