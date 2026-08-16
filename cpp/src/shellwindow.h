// dsh-desk Qt 壳主窗口：窗口内只有 QWebEngineView，对 dsh web UI 零加工。
//
// 界面原则（CLAUDE.md §1）：不加菜单栏、工具栏、状态栏，不注入 JS、不改
// User-Agent、默认缩放 100%。壳自身只有两种最小状态页（D6 已批准）：
// 启动中 / 启动失败（错误页只显示退出码与原因，不含子进程 stdout）。
#pragma once

#include <QMainWindow>

#include "dshprocess.h"  // onFailed 槽签名需要完整类型（DshProcess::FailReason）

class QWebEngineView;

class ShellWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit ShellWindow(DshProcess *dsh, QWidget *parent = nullptr);

    // 验收模式（--screenshot）需要访问渲染视图；壳自身不加工其内容。
    QWebEngineView *view() const { return m_view; }
    bool loaded() const { return m_loaded; }

protected:
    void closeEvent(QCloseEvent *event) override;  // 关窗即清树，防孤儿进程

private slots:
    void onReady(const QString &url);
    void onFailed(DshProcess::FailReason reason, const QString &message);

private:
    void showStatusPage(const QString &title, const QString &body,
                        bool progress = false);  // progress=true 显示 spinner+实时秒数

    DshProcess *m_dsh = nullptr;
    QWebEngineView *m_view = nullptr;
    bool m_loaded = false;
};
