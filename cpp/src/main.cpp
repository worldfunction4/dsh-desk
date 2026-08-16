// dsh-desk 入口：解析参数 → spawn `dsh web` → Qt WebEngine 加载。
//
// 开发期用法（checkout 根 D:\deepseek_harness\deepseek-harness）：
//   dsh-desk.exe --dsh-root D:/deepseek_harness/deepseek-harness
//   dsh-desk.exe --dsh-root ... --port 4310          # 3080 被占用时
//   dsh-desk.exe --dsh-root ... --selftest           # 内嵌回归断言
//   dsh-desk.exe --dsh-root ... --screenshot out.png # GUI 验收：截图+指标
//
// 打包期（M3）：launcher 切换为安装的 `dsh` 可执行文件。
#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWebEngineView>

#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include "dshprocess.h"
#include "selftest.h"
#include "shellwindow.h"

namespace {

// ── 渲染管线配置（Chromium flags，必须在 QApplication 创建前设置）──────
//
// 背景（真机实测 2026-08-15，notes/08）：三模式 A/B 实测（CDP rAF 帧率 +
// 滚动压力，同机同页面）——auto 58.9 FPS / hardware 60.0 FPS（零 GPU 错误
// 日志）/ software 仅 9.1 FPS（Qt 与 Chromium 双软件路径互相拖累，且
// GPU channel 反复报 "Failed to create shared context for virtualization"）。
// 早期（notes/03 §6）记录的"GPU 半失败 → 必须 software"结论已被推翻：
// 默认 flags 下 auto/hardware 均满帧。故默认 auto；software 仅作排障用。
//
// 设置面板消失修复（2026-08-15，notes/09，用户确认）：用户报告 auto 模式下
// 设置面板内移动鼠标，面板间歇性消失（动鼠标/数秒恢复）。根因：设置对话框
// 遮罩用了 backdrop-filter 毛玻璃（packages/client/ui-primitives Modal /
// ui-settings-general SettingsRoot），该合成层在本机（GameViewer 虚拟显示器
// + Qt RHI D3D11 ↔ Chromium ANGLE D3D11 双栈共享纹理 QWE_SharedImageBuffer）
// 最脆弱：hover 重绘时该层间歇性无法呈现即"面板消失"。实测对照：
// --use-angle=gl 上下文连环丢失 → QWE_SharedImageBuffer 创建失败（同族故障
// 的极端形态）；--disable-gpu-compositing 下 backdrop-filter 退化为 CPU 全屏
// 模糊 → 1 FPS 不可用；关闭 BackdropFilter 特效后 60.2 FPS 满帧且该层消失。
// 决策：默认 flags 增补 BackdropFilter 禁用（视觉代价仅遮罩毛玻璃 → 普通
// 半透明，用户已确认；不改任何 UI 代码）。
//
// 对策：
// 1. 默认 flags（对硬件渲染也无副作用，Chrome/VSCode 同款处理）：
//    - --disable-features=CalculateNativeWinOcclusion,BackdropFilter：
//      Windows 原生窗口遮挡计算是"窗口移动/切换时闪烁"的已知来源；
//      BackdropFilter 禁用见上（本机对话框消失修复）；
//    - --disable-gpu-vsync：软件合成不被 vsync 拖累；
//    - --enable-unsafe-swiftshader：软件后备完整化（WebGL 可用）。
// 2. --render 三模式（auto 默认 / software 强制软件 / hardware 强试 GPU）：
//    默认 auto（实测 58.9~60.2 FPS）；hardware（ANGLE D3D11）本机实测 60 FPS
//    满帧，作为 GPU 可用机器的候选；software 实测仅 9 FPS，仅排障用。
// 3. 用户显式设置 QTWEBENGINE_CHROMIUM_FLAGS 环境变量时，一律尊重、
//    不覆盖（官方支持的自定义通道）。

// 主参数解析前先取出 --render（QCommandLineParser 不依赖 QApplication）。
QString parseEarlyRenderMode(int argc, char *argv[]) {
    QStringList raw;
    for (int i = 1; i < argc; ++i) {
        raw << QString::fromLocal8Bit(argv[i]);
    }
    QCommandLineParser early;
    early.addOption({QStringLiteral("render"), QString(), QStringLiteral("mode")});
    early.parse(raw);  // 未知选项/错误在此忽略，主 parser 会再处理
    return early.value(QStringLiteral("render"));
}

void configureWebEngineFlags(const QString &renderMode) {
    if (!qEnvironmentVariableIsEmpty("QTWEBENGINE_CHROMIUM_FLAGS")) {
        return;  // 用户显式配置优先，壳不覆盖
    }
    QStringList flags;
    flags << QStringLiteral("--disable-features=CalculateNativeWinOcclusion,BackdropFilter")
          << QStringLiteral("--disable-gpu-vsync")
          << QStringLiteral("--enable-unsafe-swiftshader");
    if (renderMode == QLatin1String("software")) {
        flags << QStringLiteral("--disable-gpu");
        // Qt 侧也走软件 OpenGL：Qt 场景图与 Chromium(SwiftShader) 全链路
        // 一致，避免"Qt 试图与 Chromium 共享 GL 上下文"的半失败混合路径
        // （本机日志：Failed to create shared context for virtualization），
        // 该混合路径是 hover 闪烁 / 拖拽卡顿的直接来源。
        QApplication::setAttribute(Qt::AA_UseSoftwareOpenGL);
    } else if (renderMode == QLatin1String("hardware")) {
        flags << QStringLiteral("--use-gl=angle")
              << QStringLiteral("--use-angle=d3d11");
    }
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags.join(QLatin1Char(' ')).toUtf8());
}

// --log-file：把 Qt 消息同步写入文件（同时保留 stderr）。
// 用于 --selftest / --screenshot 的指标日志稳定落盘，避免 GUI 子系统下
// 句柄失效导致日志丢失（诊断教训：管道可能被残留子进程挂住）。
FILE *g_logFile = nullptr;

void fileMessageHandler(QtMsgType type, const QMessageLogContext &context,
                        const QString &msg) {
    Q_UNUSED(type);
    Q_UNUSED(context);
    const QByteArray line = msg.toUtf8() + '\n';
    if (g_logFile != nullptr) {
        fwrite(line.constData(), 1, size_t(line.size()), g_logFile);
        fflush(g_logFile);
    }
    fwrite(line.constData(), 1, size_t(line.size()), stderr);
    fflush(stderr);
}

void installFileLogger(const QString &path) {
    if (path.isEmpty()) {
        return;
    }
    g_logFile = _wfopen(reinterpret_cast<const wchar_t *>(path.utf16()), L"w");
    qInstallMessageHandler(fileMessageHandler);
}

// WIN32 子系统下：管道启动（验收/CI 场景）时 stdout/stderr 句柄有效，
// 输出经管道可捕获；仅当句柄缺失（交互式控制台）时 attach 父控制台，
// 使 --selftest / --screenshot 的指标输出可见；双击启动时无控制台窗口。
void attachParentConsole() {
#ifdef Q_OS_WIN
    if (GetStdHandle(STD_OUTPUT_HANDLE) == nullptr
        && GetStdHandle(STD_ERROR_HANDLE) == nullptr) {
        AttachConsole(ATTACH_PARENT_PROCESS);
    }
#endif
}

// 渲染内容客观证据：采样颜色数 + 主导色占比（空白页两项都会异常）。
// 与 Python 验收脚本 content_stats 同算法（步长 3）。
void contentStats(const QImage &image, int *colors, double *dominant) {
    const QImage img = image.convertToFormat(QImage::Format_RGB32);
    QHash<QRgb, int> counts;
    int total = 0;
    for (int y = 0; y < img.height(); y += 3) {
        for (int x = 0; x < img.width(); x += 3) {
            counts[img.pixel(x, y)] += 1;
            ++total;
        }
    }
    int maxCount = 0;
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
        maxCount = qMax(maxCount, it.value());
    }
    *colors = counts.size();
    *dominant = total > 0 ? double(maxCount) / double(total) : 1.0;
}

// --screenshot：真实渲染 → 截图 → 打印指标 → 退出（供验收脚本驱动）。
// --edge-shot：壳退出前用 Edge headless 对同一 URL 拍浏览器基准图
//（独立 user-data-dir 防被现有实例合并；偶发崩溃时重试 3 次）。
QString findEdge() {
    const QStringList candidates = {
        qEnvironmentVariable("DSH_DESK_EDGE"),
        QStandardPaths::findExecutable(QStringLiteral("msedge")),
        QStringLiteral("C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"),
        QStringLiteral("C:/Program Files/Microsoft/Edge/Application/msedge.exe"),
    };
    for (const QString &candidate : candidates) {
        if (!candidate.isEmpty() && QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

bool takeEdgeShot(const QString &edge, const QString &url, const QString &shotPath) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        QTemporaryDir profile(QDir::tempPath() + QStringLiteral("/dsh-edge-XXXXXX"));
        // Edge headless 退出较慢会短暂锁定 profile 目录；不自动删除，
        // 交由系统临时目录清理（避免删除失败告警干扰验收输出）。
        profile.setAutoRemove(false);
        QProcess edgeProc;
        edgeProc.start(edge,
                       {QStringLiteral("--headless=new"), QStringLiteral("--disable-gpu"),
                        QStringLiteral("--user-data-dir=%1").arg(profile.path()),
                        QStringLiteral("--screenshot=%1").arg(shotPath),
                        QStringLiteral("--window-size=1280,800"), url});
        if (edgeProc.waitForFinished(120000) && QFileInfo::exists(shotPath)) {
            return true;
        }
        QThread::msleep(1000);
    }
    return false;
}

int runScreenshotMode(QApplication &app, DshProcess *dsh, const QString &shotPath,
                      const QString &edgeShotPath) {
    ShellWindow window(dsh);
    window.show();
    dsh->start();
    QElapsedTimer elapsed;
    elapsed.start();

    const auto fail = [&](const QString &reason) {
        qInfo().noquote() << QStringLiteral("SHELL_METRICS_FAIL reason=%1").arg(reason);
        window.close();
        app.exit(2);
    };
    QObject::connect(dsh, &DshProcess::failed, &app,
                     [&](DshProcess::FailReason, const QString &message) { fail(message); });

    QObject::connect(dsh, &DshProcess::ready, &window, [&](const QString &url) {
        QWebEngineView *view = window.view();
        QObject::connect(view, &QWebEngineView::loadFinished, &window,
                         [&, url](bool ok) {
                             if (!ok) {
                                 fail(QStringLiteral("页面加载失败"));
                                 return;
                             }
                             // 等 5 秒让前端资源完全就绪再截图；软件渲染在
                             // 内存紧张时偶发白屏（colors=1），最多重试 3 次。
                             QTimer::singleShot(5000, &window, [&, url]() {
                                 QWebEngineView *v = window.view();
                                 QImage shot;
                                 int colors = 0;
                                 double dominant = 1.0;
                                 for (int attempt = 0; attempt < 3 && colors < 200; ++attempt) {
                                     if (attempt > 0) {
                                         // 嵌套事件循环等待 5s 后再抓。
                                         QEventLoop wait;
                                         QTimer::singleShot(5000, &wait, &QEventLoop::quit);
                                         wait.exec();
                                     }
                                     shot = v->grab().toImage();
                                     contentStats(shot, &colors, &dominant);
                                 }
                                 const bool saved = shot.save(shotPath);

                                 // 服务仍存活：拍浏览器基准图（壳退出前）。
                                 bool edgeOk = false;
                                 const QString edge = findEdge();
                                 if (!edge.isEmpty() && !edgeShotPath.isEmpty()) {
                                     edgeOk = takeEdgeShot(edge, url, edgeShotPath);
                                 }

                                 qInfo().noquote()
                                     << QStringLiteral(
                                            "SHELL_METRICS title=%1 colors=%2 dominant=%3 "
                                            "elapsed_ms=%4 signal=%5 url=%6 shot=%7 saved=%8 "
                                            "edge=%9 edge_exists=%10 icon_ok=%11")
                                            .arg(v->title().toHtmlEscaped())
                                            .arg(colors)
                                            .arg(dominant, 0, 'f', 2)
                                            .arg(elapsed.elapsed())
                                            .arg(dsh->readySignal() == DshProcess::ReadySignal::UrlLine
                                                     ? QStringLiteral("url_line")
                                                     : QStringLiteral("http200"))
                                            .arg(url.toHtmlEscaped())
                                            .arg(shotPath)
                                            .arg(saved ? 1 : 0)
                                            .arg(edgeShotPath)
                                            .arg(edgeOk ? 1 : 0)
                                            .arg(QApplication::windowIcon().isNull() ? 0 : 1);
                                 window.close();
                                 app.exit(saved ? 0 : 1);
                             });
                         });
    });
    return app.exec();
}

}  // namespace

int main(int argc, char *argv[]) {
    // Chromium 渲染 flags 必须在 QApplication 创建前设置（见 configureWebEngineFlags）。
    const QString renderMode = parseEarlyRenderMode(argc, argv);
    configureWebEngineFlags(renderMode);
    attachParentConsole();
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("dsh-desk"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    // 应用图标（窗口标题栏/任务栏）：PNG 版（Qt 内置格式，无插件依赖）；
    // exe 文件资源图标用同源 ICO（app.rc，资源管理器显示）。
    app.setWindowIcon(QIcon(QStringLiteral(":/dsh.png")));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("DeepSeek Harness 桌面壳（仅本机回环，UI 零加工）"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({QStringLiteral("dsh-root"),
                      QStringLiteral("dsh checkout 根目录（含 package.json）；"
                                     "默认取 $DSH_DESK_DSH_ROOT"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("host"),
                      QStringLiteral("绑定主机（默认 127.0.0.1；官方红线：禁止 0.0.0.0）"),
                      QStringLiteral("host"), QStringLiteral("127.0.0.1")});
    parser.addOption({QStringLiteral("port"),
                      QStringLiteral("监听端口（默认 0 = 由操作系统分配空闲端口；"
                                     "官方契约：pass 0 to let the OS pick a free one）"),
                      QStringLiteral("port"), QStringLiteral("0")});
    parser.addOption({QStringLiteral("timeout"),
                      QStringLiteral("就绪等待上限（毫秒，默认 180000；dsh 冷启动较慢）"),
                      QStringLiteral("ms"), QStringLiteral("180000")});
    parser.addOption({QStringLiteral("launcher"),
                      QStringLiteral("dsh 启动器覆盖（默认直启 apps/cli/lib/bin.js，"
                                     "缺失时回退 pnpm.cmd/pnpm）"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("render"),
                      QStringLiteral("渲染模式：auto（默认，Chromium 自行决定）/ "
                                     "software（强制软件渲染，GPU 半失败导致闪烁/拖拽"
                                     "卡顿时用）/ hardware（强制 ANGLE D3D11）"),
                      QStringLiteral("mode")});
    parser.addOption({QStringLiteral("selftest"),
                      QStringLiteral("运行内嵌回归断言（真实 dsh web 子进程）")});
    parser.addOption({QStringLiteral("screenshot"),
                      QStringLiteral("验收模式：渲染完成后截图到指定路径并输出指标"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("edge-shot"),
                      QStringLiteral("验收模式：壳退出前用 Edge headless 对同一 URL 截图"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("log-file"),
                      QStringLiteral("把日志同时写入指定文件（诊断/验收用）"),
                      QStringLiteral("path")});
    parser.process(app);

    installFileLogger(parser.value(QStringLiteral("log-file")));
    qInfo().noquote() << QStringLiteral("[ICON] icon_null=%1 file_exists=%2")
                             .arg(QApplication::windowIcon().isNull() ? 1 : 0)
                             .arg(QFile::exists(QStringLiteral(":/dsh.png")) ? 1 : 0);

    QString dshRoot = parser.value(QStringLiteral("dsh-root"));
    if (dshRoot.isEmpty()) {
        dshRoot = qEnvironmentVariable("DSH_DESK_DSH_ROOT");
    }
    if (dshRoot.isEmpty() || !QFileInfo(dshRoot + QStringLiteral("/package.json")).exists()) {
        qCritical().noquote()
            << QStringLiteral("错误：找不到 dsh checkout（%1）。请用 --dsh-root 指定"
                              "或设置 DSH_DESK_DSH_ROOT。")
                   .arg(dshRoot);
        return 2;
    }

    const QString host = parser.value(QStringLiteral("host"));
    // --port / --timeout 显式校验（notes/06 R7：此前 toUShort()/toInt() 对
    // 非法值静默转 0，--timeout 0 会让就绪检测立即超时）。非法即报错退出，
    // 不再静默接受。
    quint16 port = 0;
    QString portErr;
    if (!DshProcess::parsePortArg(parser.value(QStringLiteral("port")), &port, &portErr)) {
        qCritical().noquote() << portErr;
        return 2;
    }
    int timeoutMs = 180000;
    QString timeoutErr;
    if (!DshProcess::parseTimeoutArg(parser.value(QStringLiteral("timeout")), &timeoutMs,
                                     &timeoutErr)) {
        qCritical().noquote() << timeoutErr;
        return 2;
    }
    const QString launcher = parser.value(QStringLiteral("launcher"));

    if (parser.isSet(QStringLiteral("selftest"))) {
        return runSelfTest(app, dshRoot, launcher);
    }

    // 壳从不读取任何 key 文件；子进程环境 = 父进程环境原样继承（方案 A）。
    auto *dsh = new DshProcess(dshRoot, host, port, timeoutMs, launcher, &app);

    if (parser.isSet(QStringLiteral("screenshot"))) {
        return runScreenshotMode(app, dsh, parser.value(QStringLiteral("screenshot")),
                                 parser.value(QStringLiteral("edge-shot")));
    }

    ShellWindow window(dsh);
    window.show();
    dsh->start();
    return app.exec();
}
