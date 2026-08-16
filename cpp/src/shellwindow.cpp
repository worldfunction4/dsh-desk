// ShellWindow 实现。
#include "shellwindow.h"

#include <QCloseEvent>
#include <QMenuBar>
#include <QUrl>
#include <QWebEngineView>

#include "dshprocess.h"

namespace {
// 状态页模板：progress=true 时为启动等待页（图标 + spinner + 实时秒数，
// 秒数由页面内 JS 自增，C++ 无需参与）；false 为失败页（无动画）。
QString statusHtml(const QString &title, const QString &body, bool progress) {
    const QString progressHtml = progress
        ? QStringLiteral(
              "<img src=\"qrc:/dsh.png\" alt=\"\">"
              "<p>已等待 <span id=\"elapsed\">0</span> 秒…</p>"
              "<div class=\"spinner\"></div>"
              "<script>let s=0;setInterval(()=>{s++;"
              "document.getElementById('elapsed').textContent=s;},1000);</script>")
        : QString();
    return QStringLiteral(
               "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
               "<title>%1</title>"
               "<style>"
               "body{font-family:'Segoe UI',sans-serif;margin:0;height:100vh;"
               "display:flex;align-items:center;justify-content:center;"
               "background:#f7f8fa;color:#333}"
               "@media (prefers-color-scheme:dark){body{background:#1e1f24;color:#ddd}}"
               ".card{text-align:center;max-width:460px;padding:2rem}"
               "img{width:72px;height:72px;border-radius:14px;margin-bottom:1rem}"
               ".spinner{width:34px;height:34px;border:3px solid rgba(127,127,127,.25);"
               "border-top-color:#4d6bfe;border-radius:50%;margin:1.1rem auto 0;"
               "animation:spin 1s linear infinite}"
               "@keyframes spin{to{transform:rotate(360deg)}}"
               "h2{font-size:1.15rem;margin:0 0 .6rem;font-weight:600}"
               "p{font-size:.9rem;margin:.3rem 0;color:#666}"
               "@media (prefers-color-scheme:dark){p{color:#99a}}"
               "#elapsed{font-variant-numeric:tabular-nums}"
               "</style></head><body><div class=\"card\">"
               "%3"
               "<h2>%1</h2><p>%2</p>"
               "</div></body></html>")
        .arg(title.toHtmlEscaped(), body.toHtmlEscaped(), progressHtml);
}
}  // namespace

ShellWindow::ShellWindow(DshProcess *dsh, QWidget *parent)
    : QMainWindow(parent), m_dsh(dsh) {
    // 零 UI 加工：不创建菜单栏 / 工具栏 / 状态栏。
    menuBar()->setVisible(false);
    setWindowTitle(QStringLiteral("dsh-desk"));

    m_view = new QWebEngineView(this);
    setCentralWidget(m_view);
    // 默认缩放 100%，与浏览器一致；不注入 JS、不改 UA。
    m_view->setZoomFactor(1.0);
    // 页面标题同步到窗口标题（浏览器行为：标签页标题 = 页面标题）。
    connect(m_view, &QWebEngineView::titleChanged, this, [this](const QString &title) {
        setWindowTitle(title.isEmpty() ? QStringLiteral("dsh-desk") : title);
    });

    connect(m_dsh, &DshProcess::ready, this, &ShellWindow::onReady);
    connect(m_dsh, &DshProcess::failed, this, &ShellWindow::onFailed);

    // 启动状态页：就绪前显示等待提示（端口由 OS 分配时不可预知，故不显示）。
    showStatusPage(QStringLiteral("正在启动 dsh web"),
                   QStringLiteral("等待本地服务就绪…"),
                   /*progress=*/true);
    resize(1280, 800);
}

void ShellWindow::onReady(const QString &url) {
    // 就绪：加载官方本地 URL。UI 加工到此为止。
    m_loaded = true;
    m_view->setUrl(QUrl(url));
}

void ShellWindow::onFailed(DshProcess::FailReason /*reason*/, const QString &message) {
    showStatusPage(
        QStringLiteral("dsh web 启动失败"),
        QStringLiteral("%1<br>请确认 dsh checkout 可用（%2）。").arg(message, m_dsh->dshRoot()));
}

void ShellWindow::showStatusPage(const QString &title, const QString &body, bool progress) {
    m_view->setHtml(statusHtml(title, body, progress));
}

void ShellWindow::closeEvent(QCloseEvent *event) {
    // 关窗即清树：同步终止子进程树，不残留孤儿进程（AC4）。
    m_dsh->stop();
    event->accept();
}
