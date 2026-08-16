// --selftest 实现：真实 dsh web 子进程上的内嵌回归断言。
#include "selftest.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTimer>
#include <QUrl>

#include "dshprocess.h"

namespace {

// 哨兵值：仅出现在子进程环境变量里；测试断言其绝不进入壳捕获的输出。
const QString kSentinel = QStringLiteral("DSH_DESK_CPP_TEST_SENTINEL_8k2m");

struct TestReport {
    int passed = 0;
    int failed = 0;
    void check(bool ok, const QString &name, const QString &detail = QString()) {
        if (ok) {
            ++passed;
            qInfo().noquote() << QStringLiteral("[SELFTEST] PASS | %1").arg(name);
        } else {
            ++failed;
            qInfo().noquote() << QStringLiteral("[SELFTEST] FAIL | %1%2")
                                     .arg(name,
                                          detail.isEmpty() ? QString()
                                                           : QStringLiteral(" | %1").arg(detail));
        }
    }
};

int freePort() {
    QTcpServer server;
    server.listen(QHostAddress::LocalHost, 0);
    const quint16 port = server.serverPort();
    server.close();
    return port;
}

// 同步运行外部命令（Windows 匿名管道捕获；selftest 场景下与 tests/ 中
// subprocess 捕获等价，已真机验证可行）。
struct CmdResult {
    int exitCode = -1;
    QString output;
};
CmdResult runCommand(const QString &program, const QStringList &args,
                     const QString &cwd = QString(),
                     const QProcessEnvironment &extraEnv = QProcessEnvironment(),
                     int waitMs = 30000) {
    QProcess proc;
    if (!cwd.isEmpty()) {
        proc.setWorkingDirectory(cwd);
    }
    if (!extraEnv.isEmpty()) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const QString &key : extraEnv.keys()) {
            env.insert(key, extraEnv.value(key));
        }
        proc.setProcessEnvironment(env);
    }
    proc.start(program, args);
    if (!proc.waitForStarted(10000)) {
        return {-1, QStringLiteral("start failed")};
    }
    if (!proc.waitForFinished(waitMs)) {
        proc.kill();
        proc.waitForFinished(5000);
        return {-1, QStringLiteral("timeout")};
    }
    return {proc.exitCode(),
            QString::fromUtf8(proc.readAllStandardOutput())
                + QString::fromUtf8(proc.readAllStandardError())};
}

// 等 DshProcess 到达终态（ready 或 failed）。
// 注意：start() 内同步失败（如端口预检）会在本函数连接信号之前就发射
// failed，因此必须先查已定终态，再连接信号。
struct Terminal {
    bool ready = false;
    DshProcess::FailReason fail = DshProcess::FailReason::None;
};
Terminal waitTerminal(DshProcess &dsh, int timeoutMs) {
    Terminal result;
    if (dsh.readySignal() != DshProcess::ReadySignal::None) {
        result.ready = true;
        return result;
    }
    if (dsh.failReason() != DshProcess::FailReason::None) {
        result.fail = dsh.failReason();
        return result;
    }
    QEventLoop loop;
    QObject::connect(&dsh, &DshProcess::ready, &loop, [&](const QString &) {
        result.ready = true;
        loop.quit();
    });
    QObject::connect(&dsh, &DshProcess::failed, &loop,
                     [&](DshProcess::FailReason reason, const QString &) {
                         result.fail = reason;
                         loop.quit();
                     });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    return result;
}

// 同步 HTTP GET 状态码。
int httpStatus(const QString &url, int timeoutMs = 3000) {
    QNetworkAccessManager nam;
    QNetworkReply *reply = nam.get(QNetworkRequest(QUrl(url)));
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();
    return code;
}

// netstat 监听行：(本地地址, 状态)。
QList<QPair<QString, QString>> listeningRows() {
    QList<QPair<QString, QString>> rows;
    const CmdResult out = runCommand(QStringLiteral("netstat"), {QStringLiteral("-ano")});
    for (const QString &line : out.output.split(QLatin1Char('\n'))) {
        const QStringList parts = line.split(QRegularExpression(QStringLiteral("\\s+")),
                                             Qt::SkipEmptyParts);
        if (parts.size() >= 4 && parts.at(3) == QLatin1String("LISTENING")) {
            rows.append({parts.at(1), parts.at(3)});
        }
    }
    return rows;
}

bool pidAlive(qint64 pid) {
    // 用 PowerShell 判空（避免 tasklist 中文输出 GBK 编码的文本匹配问题）。
    const QString script = QStringLiteral(
        "Get-Process -Id %1 -ErrorAction SilentlyContinue | ForEach-Object { $_.Id }")
                               .arg(pid);
    const CmdResult out = runCommand(QStringLiteral("powershell"),
                                     {QStringLiteral("-NoProfile"), QStringLiteral("-Command"),
                                      script},
                                     QString(), QProcessEnvironment(), 30000);
    return !out.output.trimmed().isEmpty();
}

// dsh 残留判定：该 PID 存活**且**命令行同时含 `web` 与该端口特征。
// 裸 PID 存活会被 Windows 快速 PID 复用误报（实测：冲突实例死亡后其 PID
// 被新进程占用，导致误判残留），因此必须校验命令行身份。
bool dshLeftoverAlive(qint64 pid, quint16 port) {
    const QString script = QStringLiteral(
        "Get-CimInstance Win32_Process -Filter \"ProcessId=%1\" | "
        "Where-Object { $_.CommandLine -match 'web' -and $_.CommandLine -match '--port %2' } | "
        "ForEach-Object { $_.ProcessId }")
                               .arg(pid)
                               .arg(port);
    const CmdResult out = runCommand(QStringLiteral("powershell"),
                                     {QStringLiteral("-NoProfile"), QStringLiteral("-Command"),
                                      script},
                                     QString(), QProcessEnvironment(), 60000);
    return !out.output.trimmed().isEmpty();
}

// 按命令行特征查找监听该端口的 dsh web node 进程（与 tests/util.py 同逻辑）。
QList<qint64> nodePidsForPort(quint16 port) {
    const QString script = QStringLiteral(
        "Get-CimInstance Win32_Process -Filter \"Name='node.exe'\" | "
        "Where-Object { $_.CommandLine -match 'web' -and $_.CommandLine -match '--port %1' } | "
        "ForEach-Object { $_.ProcessId }")
                               .arg(port);
    const CmdResult out = runCommand(QStringLiteral("powershell"),
                                     {QStringLiteral("-NoProfile"), QStringLiteral("-Command"),
                                      script},
                                     QString(), QProcessEnvironment(), 60000);
    QList<qint64> pids;
    for (const QString &part : out.output.split(QRegularExpression(QStringLiteral("\\s+")),
                                                Qt::SkipEmptyParts)) {
        bool ok = false;
        const qint64 pid = part.toLongLong(&ok);
        if (ok) {
            pids.append(pid);
        }
    }
    return pids;
}

QString tempDshHome() {
    QDir dir = QDir::temp();
    const QString path = dir.filePath(QStringLiteral("dsh-desk-selftest-%1")
                                          .arg(QCoreApplication::applicationPid()));
    QDir().mkpath(path);
    return path;
}

// 隔离环境：临时 DSH_HOME + 哨兵 key（只进环境，不落盘）。
QProcessEnvironment isolatedEnv(const QString &dshHome) {
    QProcessEnvironment env;
    env.insert(QStringLiteral("DSH_HOME"), dshHome);
    env.insert(QStringLiteral("DEEPSEEK_API_KEY"), kSentinel);
    return env;
}

}  // namespace

// 解析 pnpm 启动器（.cmd shim 路径），供 pnpm 回退链路用例使用。
QString resolvePnpmLauncher() {
    const QString pnpmCmd = QStandardPaths::findExecutable(QStringLiteral("pnpm.cmd"));
    if (!pnpmCmd.isEmpty()) {
        return pnpmCmd;
    }
    return QStandardPaths::findExecutable(QStringLiteral("pnpm"));
}

// 默认启动命令：与 DshProcess::buildLaunchSpec 同逻辑（lib 直启 → pnpm 回退）。
// 返回 (program, prefix)；调用处追加 `web --host ... --port ...`。
struct LaunchCommand {
    QString program;
    QStringList prefix;
};
LaunchCommand defaultLaunchCommand(const QString &dshRoot) {
    QString node = QStandardPaths::findExecutable(QStringLiteral("node.exe"));
    if (node.isEmpty()) {
        node = QStandardPaths::findExecutable(QStringLiteral("node"));
    }
    if (!node.isEmpty()) {
        const QString libBin = dshRoot + QStringLiteral("/apps/cli/lib/bin.js");
        if (QFileInfo::exists(libBin)) {
            return {node, {libBin}};
        }
        const QString launcher = resolvePnpmLauncher();
        if (launcher.endsWith(QLatin1String(".cmd"), Qt::CaseInsensitive)
            || launcher.endsWith(QLatin1String(".bat"), Qt::CaseInsensitive)) {
            const QFileInfo shim(launcher);
            const QString pnpmMjs = shim.dir().filePath(
                QStringLiteral("node_modules/pnpm/bin/pnpm.mjs"));
            if (QFileInfo::exists(pnpmMjs)) {
                return {node, {pnpmMjs, QStringLiteral("dsh")}};
            }
        }
        if (!launcher.isEmpty()) {
            return {launcher, {}};
        }
    }
    return {};
}

int runSelfTest(QCoreApplication & /*app*/, const QString &dshRoot, const QString &launcher) {
    TestReport report;
    const QString dshHome = tempDshHome();

    // launcher 仅当用户显式指定时非空；默认走 DshProcess 内置探测
    // （lib 直启优先，pnpm 回退），与壳真实启动路径一致。
    const QString effectiveLauncher = launcher;

    qInfo().noquote() << QStringLiteral("[SELFTEST] dshRoot=%1 launcher=%2")
                             .arg(dshRoot, effectiveLauncher);

    // ── 1. 一次真实冷启动覆盖：就绪 / 回环 / 密钥卫生 / 进程树清理 ──────
    {
        const int port = freePort();
        DshProcess dsh(dshRoot, QStringLiteral("127.0.0.1"), port, 180000, effectiveLauncher);
        dsh.setExtraEnv(isolatedEnv(dshHome));
        dsh.start();
        const Terminal terminal = waitTerminal(dsh, 200000);

        report.check(terminal.ready, QStringLiteral("1a 就绪（官方信号）"),
                     terminal.ready ? dsh.url()
                                    : QStringLiteral("reason=%1").arg(int(terminal.fail)));
        report.check(dsh.readySignal() == DshProcess::ReadySignal::UrlLine,
                     QStringLiteral("1b 主信号=官方 URL 行"),
                     QStringLiteral("signal=%1").arg(int(dsh.readySignal())));
        report.check(dsh.url() == QStringLiteral("http://127.0.0.1:%1").arg(port),
                     QStringLiteral("1c URL 端口匹配"), dsh.url());
        report.check(httpStatus(dsh.url()) == 200, QStringLiteral("1d HTTP 200"), dsh.url());

        // 密钥卫生：哨兵绝不出现在壳捕获的输出（AC6）。
        bool leaked = false;
        for (const QString &line : dsh.logLines()) {
            if (line.contains(kSentinel)) {
                leaked = true;
                break;
            }
        }
        report.check(!leaked && !dsh.logLines().isEmpty(),
                     QStringLiteral("1e 哨兵不进壳输出"));

        // 回环绑定（AC3）：真实监听表只允许 127.0.0.1。
        bool loopback = false;
        bool wildcard = false;
        const QString want = QStringLiteral("127.0.0.1:%1").arg(port);
        const QString bad = QStringLiteral("0.0.0.0:%1").arg(port);
        for (const auto &row : listeningRows()) {
            loopback |= row.first == want;
            wildcard |= row.first == bad;
        }
        report.check(loopback, QStringLiteral("1f 仅 127.0.0.1 监听"));
        report.check(!wildcard, QStringLiteral("1g 无 0.0.0.0 监听"));

        // 进程树清理（AC4）：stop 前后两层核对。
        const qint64 pid = dsh.pid();
        const QList<qint64> nodesBefore = nodePidsForPort(port);
        report.check(pidAlive(pid), QStringLiteral("1h stop 前根进程存活"));
        report.check(!nodesBefore.isEmpty(), QStringLiteral("1i stop 前 node 进程存在"));
        dsh.stop();
        report.check(!dshLeftoverAlive(pid, port), QStringLiteral("1j stop 后根进程消失"));
        report.check(nodePidsForPort(port).isEmpty(), QStringLiteral("1k stop 后无残留 node"));
    }

    // ── 2. 红线契约：--host 0.0.0.0 必须被官方拒绝 ───────────────────────
    {
        const int port = freePort();
        const LaunchCommand cmd = defaultLaunchCommand(dshRoot);
        QStringList args = cmd.prefix;
        args << QStringLiteral("web")
             << QStringLiteral("--host") << QStringLiteral("0.0.0.0")
             << QStringLiteral("--port") << QString::number(port);
        const CmdResult out = runCommand(cmd.program, args, dshRoot, isolatedEnv(dshHome),
                                         200000);
        const QString combined = out.output;
        report.check(out.exitCode != 0, QStringLiteral("2a 0.0.0.0 非零退出"),
                     QStringLiteral("exit=%1").arg(out.exitCode));
        report.check(combined.contains(QStringLiteral(
                         "it would expose remote code execution to the network; "
                         "use 127.0.0.1 instead")),
                     QStringLiteral("2b 官方报错原文命中"));
    }

    // ── 3. 端口冲突：干净失败（AC5） ─────────────────────────────────────
    {
        QTcpServer blocker;
        blocker.listen(QHostAddress::LocalHost, 0);
        const quint16 port = blocker.serverPort();

        DshProcess dsh(dshRoot, QStringLiteral("127.0.0.1"), port, 180000, effectiveLauncher);
        dsh.setExtraEnv(isolatedEnv(dshHome));
        dsh.start();
        const Terminal terminal = waitTerminal(dsh, 200000);
        report.check(!terminal.ready && terminal.fail == DshProcess::FailReason::ProcessExited,
                     QStringLiteral("3a 端口冲突→进程退出失败"),
                     QStringLiteral("ready=%1 fail=%2").arg(terminal.ready).arg(int(terminal.fail)));
        const qint64 pid = dsh.pid();
        dsh.stop();
        report.check(!dshLeftoverAlive(pid, port), QStringLiteral("3b 冲突路径无残留"));
        blocker.close();
    }

    // ── 4. 就绪超时：干净失败（AC5） ─────────────────────────────────────
    {
        const int port = freePort();
        DshProcess dsh(dshRoot, QStringLiteral("127.0.0.1"), port, 500, effectiveLauncher);
        dsh.setExtraEnv(isolatedEnv(dshHome));
        dsh.start();
        const Terminal terminal = waitTerminal(dsh, 30000);
        report.check(!terminal.ready && terminal.fail == DshProcess::FailReason::Timeout,
                     QStringLiteral("4a 极小超时→Timeout 失败"));
        const qint64 pid = dsh.pid();
        dsh.stop();
        report.check(!dshLeftoverAlive(pid, port), QStringLiteral("4b 超时路径无残留"));
    }

    // ── 5. 壳自身防线：非回环 host 构造即拒 ──────────────────────────────
    {
        report.check(!DshProcess::isLoopbackHost(QStringLiteral("0.0.0.0")),
                     QStringLiteral("5a 0.0.0.0 判定非回环"));
        report.check(DshProcess::isLoopbackHost(QStringLiteral("127.0.0.1"))
                         && DshProcess::isLoopbackHost(QStringLiteral("localhost"))
                         && DshProcess::isLoopbackHost(QStringLiteral("::1")),
                     QStringLiteral("5b 回环地址判定"));
        DshProcess guarded(dshRoot, QStringLiteral("0.0.0.0"), freePort(), 1000, effectiveLauncher);
        report.check(guarded.errorString().contains(QStringLiteral("回环")),
                     QStringLiteral("5c 构造期拒绝非回环 host"));
    }

    // ── 6. --port 0：操作系统分配空闲端口（官方契约 startup.ts:49） ────
    {
        DshProcess dsh(dshRoot, QStringLiteral("127.0.0.1"), 0, 180000, effectiveLauncher);
        dsh.setExtraEnv(isolatedEnv(dshHome));
        dsh.start();
        const Terminal terminal = waitTerminal(dsh, 200000);
        report.check(terminal.ready, QStringLiteral("6a --port 0 就绪（官方 URL 行）"));
        report.check(dsh.boundPort() != 0, QStringLiteral("6b OS 实际分配端口非零"),
                     QStringLiteral("bound=%1").arg(dsh.boundPort()));
        bool loopback = false;
        const QString want = QStringLiteral("127.0.0.1:%1").arg(dsh.boundPort());
        for (const auto &row : listeningRows()) {
            loopback |= row.first == want;
        }
        report.check(loopback, QStringLiteral("6c 实际端口仅 127.0.0.1 监听"));
        const qint64 pid = dsh.pid();
        dsh.stop();
        report.check(!dshLeftoverAlive(pid, dsh.boundPort()),
                     QStringLiteral("6d 退出后 OS 分配端口释放、无残留"));
    }

    // ── 7. pnpm 回退链路（lib/ 未构建的 checkout 用）：仍可就绪、可清理 ──
    {
        const QString pnpmLauncher = resolvePnpmLauncher();
        report.check(!pnpmLauncher.isEmpty(), QStringLiteral("7a 找到 pnpm 启动器"));
        if (!pnpmLauncher.isEmpty()) {
            DshProcess dsh(dshRoot, QStringLiteral("127.0.0.1"), 0, 180000, pnpmLauncher);
            dsh.setExtraEnv(isolatedEnv(dshHome));
            dsh.start();
            const Terminal terminal = waitTerminal(dsh, 200000);
            report.check(terminal.ready, QStringLiteral("7b pnpm 回退链路就绪（--port 0）"),
                         QStringLiteral("ready=%1 fail=%2").arg(terminal.ready)
                             .arg(int(terminal.fail)));
            const qint64 pid = dsh.pid();
            dsh.stop();
            report.check(!dshLeftoverAlive(pid, dsh.boundPort()),
                         QStringLiteral("7c pnpm 回退链路退出无残留"));
        }
    }

    // ── 8. CLI 输入校验（notes/06 R7 修复：非法值不再静默转 0） ──────────
    {
        quint16 port = 0;
        QString err;
        report.check(DshProcess::parsePortArg(QStringLiteral("0"), &port, &err) && port == 0,
                     QStringLiteral("8a --port 0（OS 分配）合法"));
        report.check(DshProcess::parsePortArg(QStringLiteral("4310"), &port, &err)
                         && port == 4310,
                     QStringLiteral("8b --port 4310 合法"));
        report.check(!DshProcess::parsePortArg(QStringLiteral("abc"), &port, &err)
                         && !err.isEmpty(),
                     QStringLiteral("8c --port 非数字拒绝"), err);
        report.check(!DshProcess::parsePortArg(QStringLiteral("-1"), &port, &err),
                     QStringLiteral("8d --port 负数拒绝"));
        report.check(!DshProcess::parsePortArg(QStringLiteral("65536"), &port, &err),
                     QStringLiteral("8e --port 超上界拒绝"));

        int timeout = 0;
        report.check(DshProcess::parseTimeoutArg(QStringLiteral("180000"), &timeout, &err)
                         && timeout == 180000,
                     QStringLiteral("8f --timeout 180000 合法"));
        report.check(!DshProcess::parseTimeoutArg(QStringLiteral("0"), &timeout, &err),
                     QStringLiteral("8g --timeout 0 拒绝（否则立即超时）"));
        report.check(!DshProcess::parseTimeoutArg(QStringLiteral("-5"), &timeout, &err),
                     QStringLiteral("8h --timeout 负数拒绝"));
        report.check(!DshProcess::parseTimeoutArg(QStringLiteral("abc"), &timeout, &err),
                     QStringLiteral("8i --timeout 非数字拒绝"));
    }

    qInfo().noquote() << QStringLiteral("[SELFTEST] SUMMARY: %1 passed, %2 failed")
                             .arg(report.passed)
                             .arg(report.failed);
    return report.failed == 0 ? 0 : 1;
}
