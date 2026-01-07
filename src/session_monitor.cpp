/*
 * session_monitor.cpp
 * Session 監控器停駐窗口實現
 */

#include "session_monitor.h"

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QMainWindow>
#include <QHeaderView>
#include <QFont>

#include "flow_meter.h"

static SessionMonitorDock* g_monitor_dock = nullptr;

// ========== SessionMonitorDock ==========
SessionMonitorDock::SessionMonitorDock(QWidget* parent)
    : QDockWidget("Remote Capture Monitor", parent)
    , timer_(nullptr)
    , table_(nullptr)
    , summary_label_(nullptr)
{
    setObjectName("RemoteCaptureMonitorDock");
    setAllowedAreas(Qt::AllDockWidgetAreas);
    setFeatures(QDockWidget::DockWidgetClosable | 
                QDockWidget::DockWidgetMovable | 
                QDockWidget::DockWidgetFloatable);
    
    // 創建主容器
    QWidget* container = new QWidget(this);
    QVBoxLayout* layout = new QVBoxLayout(container);
    layout->setContentsMargins(5, 5, 5, 5);
    layout->setSpacing(5);
    
    // 總覽標籤
    summary_label_ = new QLabel("Sessions: 0 | Total: 0 B/s", container);
    summary_label_->setStyleSheet("font-weight: bold; padding: 5px;");
    layout->addWidget(summary_label_);
    
    // Session 表格
    table_ = new QTableWidget(container);
    table_->setColumnCount(7);
    table_->setHorizontalHeaderLabels({
        "Session ID", "Source Type", "Encoder", "Protocol", "Size", "Frame", "Rate"
    });
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->verticalHeader()->setVisible(false);
    layout->addWidget(table_);
    
    setWidget(container);
    
    // 每秒更新
    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &SessionMonitorDock::updateStatus);
    timer_->start(1000);
    
    // 初始更新
    updateStatus();
}

SessionMonitorDock::~SessionMonitorDock() {
    if (timer_) {
        timer_->stop();
    }
}

void SessionMonitorDock::updateStatus() {
    std::vector<SessionStatus> sessions = grpc_server_get_session_status();
    
    // 更新表格
    table_->setRowCount((int)sessions.size());
    
    double total_rate = 0;
    int streaming_count = 0;
    
    for (int i = 0; i < (int)sessions.size(); i++) {
        const auto& s = sessions[i];
        
        // Session ID (縮短顯示)
        QString short_id = QString::fromStdString(s.id);
        if (short_id.length() > 8) {
            short_id = short_id.left(8) + "...";
        }
        
        // 從 FlowMeter 獲取該 stream 的實際速率
        double rate = g_flow_meter.getStreamRate(s.stream_id);
        
        table_->setItem(i, 0, new QTableWidgetItem(short_id));
        table_->setItem(i, 1, new QTableWidgetItem(QString::fromStdString(s.source_type)));
        table_->setItem(i, 2, new QTableWidgetItem(QString::fromStdString(s.encoder_name)));
        table_->setItem(i, 3, new QTableWidgetItem(QString::fromStdString(s.protocol)));
        table_->setItem(i, 4, new QTableWidgetItem(
            QString("%1x%2").arg(s.width).arg(s.height)));
        table_->setItem(i, 5, new QTableWidgetItem(QString::number(s.frame_count)));
        table_->setItem(i, 6, new QTableWidgetItem(FlowMeter::formatRate(rate)));
        
        // 設定串流狀態顏色
        QColor row_color = s.streaming ? QColor(42, 176, 49) : QColor(199, 60, 50);
        for (int j = 0; j < 7; j++) {
            if (table_->item(i, j)) {
                table_->item(i, j)->setBackground(row_color);
            }
        }
        
        total_rate += rate;
        if (s.streaming) streaming_count++;
    }
    
    // 更新總覽 (使用 FlowMeter 的格式化函數)
    summary_label_->setText(QString("Sessions: %1 (%2 streaming) | Total: %3")
        .arg(sessions.size())
        .arg(streaming_count)
        .arg(FlowMeter::formatRate(total_rate)));
}

// ========== 公開函數 (C 連結) ==========
extern "C" {

void session_monitor_init(void) {
    obs_queue_task(OBS_TASK_UI, [](void*) {
        QMainWindow* main_window = (QMainWindow*)obs_frontend_get_main_window();
        if (main_window) {
            g_monitor_dock = new SessionMonitorDock(main_window);
            
            // 使用 obs_frontend_add_custom_qdock (OBS 30.0+)
            // 這個 API 用於添加自定義 QDockWidget，不會在 Docks 菜單顯示切換選項
            if (!obs_frontend_add_custom_qdock("remote_capture_monitor", g_monitor_dock)) {
                // 如果 ID 已存在，手動添加到主窗口
                main_window->addDockWidget(Qt::RightDockWidgetArea, g_monitor_dock);
            }
            
            blog(LOG_INFO, "[Session Monitor] Dock widget created");
        }
    }, nullptr, false);
}

void session_monitor_cleanup(void) {
    // 使用 obs_frontend_remove_dock 移除 (OBS 30.0+)
    obs_frontend_remove_dock("remote_capture_monitor");
    g_monitor_dock = nullptr;  // Widget 由 OBS 管理
}

}  // extern "C"
