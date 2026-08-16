// DshProcess 实现：spawn / 双信号就绪检测 / 进程树清理。
#include "dshprocess.h"

#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QUrl>

namespace {
// 官方就绪信号行：`dsh web: http://127.0.0.1:3080`（可能带 `(LAN: ...)` 后缀）。
const QRegularExpression kUrlLineRe(QStringLiteral(R"(dsh web:\s+(http://[^\s]+))"));
// 兜底就绪：HTTP 200 连续稳定出现该次数才判定。
constexpr int kHttpStableCount = 3;
constexpr int kLogMaxLines = 200;
}  // namespace

DshProcess::DshProcess(const QString &dshRoot, const QString &host, quint16 port,
                       int timeoutMs, const QString &launcher, QObject *parent)
    : QObject(parent),
      m_dshRoot(dshRoot),
      m_host(host),
      m_launcher(launcher),
      m_port(port),
      m_timeoutMs(timeoutMs) {
    // 壳自身的第一道防线：非回环 host 在构造期即拒绝（官方红线）。
    if (!isLoopbackHost(m_host)) {
        m_error = QStringLiteral("拒绝非回环 host: %1（官方红线：仅本机回环）").arg(m_host);
        m_failReason = FailReason::ProcessExited;
        return;
    }

    // stdout 与 stderr 合并捕获（内存环形缓冲，绝不落盘）。
    m_process.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &DshProcess::onStdout);
    connect(&m_process, &QProcess::finished, this, &DshProcess::onProcessFinished);

    m_httpTimer.setInterval(400);
    connect(&m_httpTimer, &QTimer::timeout, this, &DshProcess::onHttpPoll);
    m_timeoutTimer.setSingleShot(true);
    connect(&m_timeoutTimer, &QTimer::timeout, this, &DshProcess::onTimeout);
}

DshProcess::~DshProcess() {
#ifdef Q_OS_WIN
    // 关闭 Job 句柄即销毁 Job：KILL_ON_JOB_CLOSE 使整棵进程树随之终结。
    // 无论壳正常退出还是异常崩溃，OS 都会代劳清理，杜绝孤儿进程。
    if (m_job != nullptr) {
        CloseHandle(m_job);
        m_job = nullptr;
    }
#endif
}

bool DshProcess::isLoopbackHost(const QString &host) {
    return host == QLatin1String("127.0.0.1") || host == QLatin1String("localhost")
        || host == QLatin1String("::1");
}

// --port 输入校验（notes/06 R7 修复：此前 toUShort() 非法值静默转 0）：
// 合法值 = 0（OS 分配空闲端口，官方契约）或 1–65535。
bool DshProcess::parsePortArg(const QString &raw, quint16 *out, QString *err) {
    bool ok = false;
    const uint value = raw.toUInt(&ok);
    if (!ok || value > 65535u) {
        if (err != nullptr) {
            *err = QStringLiteral("无效的 --port 值 '%1'（应为 0=OS 分配，或 1–65535）")
                       .arg(raw);
        }
        return false;
    }
    *out = quint16(value);
    return true;
}

// --timeout 输入校验：合法值 = 正整数毫秒（>0；0 会让 QTimer::start(0) 立即超时，
// 负数/非数字无意义）。
bool DshProcess::parseTimeoutArg(const QString &raw, int *out, QString *err) {
    bool ok = false;
    const int value = raw.toInt(&ok);
    if (!ok || value <= 0) {
        if (err != nullptr) {
            *err = QStringLiteral("无效的 --timeout 值 '%1'（应为正整数毫秒，>0）")
                       .arg(raw);
        }
        return false;
    }
    *out = value;
    return true;
}

void DshProcess::setExtraEnv(const QProcessEnvironment &env) {
    m_extraEnv = env;
}

// 显式 `--launcher` 覆盖；未指定时回退探测 pnpm（lib/ 未构建的 checkout）。
QString DshProcess::resolveLauncher() const {
    if (!m_launcher.isEmpty()) {
        return m_launcher;
    }
    // 开发期：pnpm.cmd（checkout 的 `dsh` script）；打包期切换安装的 dsh 可执行文件。
    const QString pnpmCmd = QStandardPaths::findExecutable(QStringLiteral("pnpm.cmd"));
    if (!pnpmCmd.isEmpty()) {
        return pnpmCmd;
    }
    return QStandardPaths::findExecutable(QStringLiteral("pnpm"));
}

// 启动命令构造。
//
// 默认优先直启 dsh 的构建产物 `apps/cli/lib/bin.js`（npm 安装版的正式
// 入口，package.json `bin` 字段），理由（均真机实测，同环境对照）：
//   - pnpm 链路（`pnpm dsh` → `node --import tsx/esm apps/cli/src/bin.ts`）
//     每次启动都经 tsx/esbuild 实时转译 TS 源码，实测就绪 60s+（tsx
//     进程满负荷）；直启构建产物实测 8.7s，提速约 7 倍。
//   - 直启 = npm 正式安装形态（M3 打包后目标机器没有 pnpm/tsx 也一样跑）。
//   - 仍保留 pnpm 回退：checkout 未构建（lib/ 缺失）时退回原链路。
//
// 实测教训（保留）：直接 QProcess 启动 pnpm.cmd 会引入 cmd.exe 包装层，
// 该层可能在 node 树之前退出，导致 QProcess 误判"进程已结束"、taskkill
// /T 失去根节点而留下孤儿 node（真机复现：两个残留实例的 cmd 父已死）。
// 因此一律直接 spawn `node`：QProcess 持有的是真实存活的 node 根进程，
// 其存活期与 dsh web 一致，taskkill /T 树清理可靠。
DshProcess::LaunchSpec DshProcess::buildLaunchSpec() const {
    QString node = QStandardPaths::findExecutable(QStringLiteral("node.exe"));
    if (node.isEmpty()) {
        node = QStandardPaths::findExecutable(QStringLiteral("node"));
    }
    if (node.isEmpty()) {
        return {};  // 无 node：下面统一报错
    }

    // 首选：构建产物直启（m_dshRoot/apps/cli/lib/bin.js，CLI 入口直接收 `web`）。
    const QString libBin = m_dshRoot + QStringLiteral("/apps/cli/lib/bin.js");
    if (QFileInfo::exists(libBin)) {
        return {node, {libBin}};
    }

    // 回退：pnpm 链路（checkout 未构建 lib/ 时；launcher 显式指定时除外）。
    // pnpm 侧需要 `dsh` 一词（根 package.json 的 `dsh` script），故放入 prefix。
    const QString launcher = resolveLauncher();
    if (launcher.endsWith(QLatin1String(".cmd"), Qt::CaseInsensitive)
        || launcher.endsWith(QLatin1String(".bat"), Qt::CaseInsensitive)) {
        const QFileInfo shim(launcher);
        const QString pnpmMjs = shim.dir().filePath(
            QStringLiteral("node_modules/pnpm/bin/pnpm.mjs"));
        if (QFileInfo::exists(pnpmMjs)) {
            return {node, {pnpmMjs, QStringLiteral("dsh")}};
        }
    }
    return {launcher, {}};
}

void DshProcess::start() {
    if (m_failReason != FailReason::None && m_readySignal == ReadySignal::None) {
        // 构造期已判定失败（如非回环 host）：此刻接收者已连接，补发信号。
        m_httpTimer.stop();
        m_timeoutTimer.stop();
        emit failed(m_failReason, m_error);
        return;
    }
    const LaunchSpec spec = buildLaunchSpec();
    if (spec.program.isEmpty()) {
        markFailed(FailReason::ProcessExited,
                   QStringLiteral("找不到 Node.js / dsh 启动入口：请确认 Node.js 已安装，"
                                  "且 dsh checkout 可用（lib/bin.js 或 pnpm）"));
        return;
    }

    // 端口占用预检：避免 HTTP 兜底命中"别人的服务"（实测教训：3080 被
    // 用户会话占用时，兜底会连上他人实例）。连接成功即视为已占用。
    // --port 0（OS 分配）无需预检：操作系统保证分配空闲端口。
    if (m_port != 0) {
        QTcpSocket probe;
        probe.connectToHost(m_host, m_port);
        if (probe.waitForConnected(500)) {
            probe.disconnectFromHost();
            markFailed(FailReason::ProcessExited,
                       QStringLiteral("端口 %1 已被占用（http://%2:%1）").arg(m_port).arg(m_host));
            return;
        }
    }

    // 继承父进程环境（方案 A：API key 由用户配置在系统环境变量中，
    // 壳全程不读、不打印、不写任何 key 文件）；测试注入仅经 setExtraEnv。
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const QString &key : m_extraEnv.keys()) {
        env.insert(key, m_extraEnv.value(key));
    }
    m_process.setProcessEnvironment(env);

    m_process.setWorkingDirectory(m_dshRoot);
    // prefix 已含直达 `web` 所需前缀（lib 直启 / pnpm dsh / launcher）。
    QStringList args = spec.prefix;
    args << QStringLiteral("web")
         << QStringLiteral("--host") << m_host
         << QStringLiteral("--port") << QString::number(m_port);
    m_process.start(spec.program, args);

    if (!m_process.waitForStarted(10000)) {
        markFailed(FailReason::ProcessExited,
                   QStringLiteral("无法启动 dsh web（%1）：%2")
                       .arg(spec.program, m_process.errorString()));
        return;
    }

#ifdef Q_OS_WIN
    // 把子进程挂进 kill-on-close Job（防线 1）：壳无论以何种方式退出
    //（含崩溃），Windows 都会自动终结整棵进程树。此前实测：closeEvent
    // 同步清理路径下 exe 崩溃时 node 树全部残留为孤儿。
    m_job = CreateJobObjectW(nullptr, nullptr);
    if (m_job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &info,
                                sizeof(info));
        const HANDLE proc = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                                        DWORD(m_process.processId()));
        if (proc != nullptr) {
            AssignProcessToJobObject(m_job, proc);
            CloseHandle(proc);
        }
    }
#endif
    m_timeoutTimer.start(m_timeoutMs);
    m_httpTimer.start();
}

void DshProcess::onStdout() {
    // 逐行消费合并输出：捕捉官方 URL 行，其余进内存环形缓冲。
    const QByteArray chunk = m_process.readAllStandardOutput();
    m_pendingLine += QString::fromUtf8(chunk);
    int pos = 0;
    while ((pos = m_pendingLine.indexOf(QLatin1Char('\n'))) >= 0) {
        QString line = m_pendingLine.left(pos).remove(QLatin1Char('\r'));
        m_pendingLine.remove(0, pos + 1);
        m_log.append(line);
        while (m_log.size() > kLogMaxLines) {
            m_log.removeFirst();
        }
        const auto match = kUrlLineRe.match(line);
        if (match.hasMatch() && m_readySignal == ReadySignal::None) {
            m_url = match.captured(1);
            // --port 0 时 OS 分配的实际端口从官方 URL 行解析（url 形如
            // http://127.0.0.1:<实际端口>）；显式端口时两者一致。
            const QRegularExpression portRe(QStringLiteral(":(\\d+)$"));
            const auto portMatch = portRe.match(m_url);
            if (portMatch.hasMatch()) {
                m_boundPort = quint16(portMatch.captured(1).toUInt());
            }
            markReady(ReadySignal::UrlLine);
            return;
        }
    }
}

void DshProcess::onProcessFinished(int code, QProcess::ExitStatus /*status*/) {
    m_finished = true;
    m_httpTimer.stop();
    m_timeoutTimer.stop();
    // 输出中残存的最后一行也保留（可能包含错误原因）。
    if (!m_pendingLine.isEmpty()) {
        m_log.append(m_pendingLine);
        m_pendingLine.clear();
    }
    // 尚未就绪即退出 → 失败（就绪前退出是启动失败，不是正常关停）。
    if (m_readySignal == ReadySignal::None && m_failReason == FailReason::None) {
        markFailed(FailReason::ProcessExited,
                   QStringLiteral("dsh web 在就绪前退出（退出码 %1）").arg(code));
    }
}

void DshProcess::onHttpPoll() {
    // 官方信号优先，HTTP 200 仅作兜底（printUrl 被关闭时仍可判定）；
    // 兜底只认"自身进程仍在运行"时的服务，防命中他人实例（竞态窗口）。
    // --port 0（OS 分配）时无固定端口可探，且官方 URL 行必然打印（壳
    // 依赖它获知实际端口），故仅靠 URL 行主信号。
    if (m_readySignal != ReadySignal::None || m_reply != nullptr
        || m_process.state() != QProcess::Running || m_port == 0) {
        return;
    }
    QNetworkRequest request(QUrl(QStringLiteral("http://%1:%2/").arg(m_host).arg(m_port)));
    request.setTransferTimeout(1000);
    m_reply = m_nam.get(request);
    connect(m_reply, &QNetworkReply::finished, this, &DshProcess::onHttpReplyFinished);
}

void DshProcess::onHttpReplyFinished() {
    if (m_reply == nullptr) {
        return;
    }
    const bool ok = m_reply->error() == QNetworkReply::NoError
        && m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
    m_reply->deleteLater();
    m_reply = nullptr;
    if (m_readySignal != ReadySignal::None) {
        return;
    }
    // 自身进程已退出时的 200 不算数（可能命中端口复用的他人实例）。
    const bool own = m_process.state() == QProcess::Running;
    m_httpStreak = (ok && own) ? m_httpStreak + 1 : 0;
    if (m_httpStreak >= kHttpStableCount) {
        m_url = QStringLiteral("http://%1:%2").arg(m_host).arg(m_port);
        m_boundPort = m_port;
        markReady(ReadySignal::Http200);
    }
}

void DshProcess::onTimeout() {
    m_httpTimer.stop();
    if (m_readySignal == ReadySignal::None && m_failReason == FailReason::None) {
        markFailed(FailReason::Timeout,
                   QStringLiteral("等待就绪超时（%1ms）：未观察到官方 `dsh web:` URL 行，"
                                  "也未得到稳定的 HTTP 200")
                       .arg(m_timeoutMs));
    }
}

void DshProcess::markReady(ReadySignal signal) {
    if (m_readySignal != ReadySignal::None || m_failReason != FailReason::None) {
        return;  // 终态幂等：ready/failed 只发生一次。
    }
    m_readySignal = signal;
    m_httpTimer.stop();
    m_timeoutTimer.stop();
    emit ready(m_url);
}

void DshProcess::markFailed(FailReason reason, const QString &message) {
    if (m_readySignal != ReadySignal::None || m_failReason != FailReason::None) {
        return;  // 终态幂等：ready/failed 只发生一次。
    }
    m_failReason = reason;
    m_error = message;
    m_httpTimer.stop();
    m_timeoutTimer.stop();
    emit failed(reason, message);
}

void DshProcess::stop(int waitMs) {
    if (m_process.state() == QProcess::NotRunning) {
        return;
    }
    const qint64 pid = m_process.processId();
    qInfo().noquote() << QStringLiteral("[STOP] pid=%1 state=%2").arg(pid).arg(int(m_process.state()));
    // Windows：pnpm 树是多层进程，仅终止直接子进程会残留 node。
    // 用 startDetached 异步 taskkill（防线 2）：不在 closeEvent 里同步阻塞
    // 等待 taskkill 子进程（此前实测该同步等待在 WebEngine 关闭链中导致
    // exe 崩溃、整树残留）。Job Object（防线 1）兜底保证最终无孤儿。
#ifdef Q_OS_WIN
    QProcess::startDetached(QStringLiteral("taskkill"),
                            {QStringLiteral("/PID"), QString::number(pid),
                             QStringLiteral("/T"), QStringLiteral("/F")});
#endif
    // 分段等待树死亡（每次 500ms 泵事件，上限 waitMs）。
    const int slices = (waitMs + 499) / 500;
    for (int i = 0; i < slices && m_process.state() != QProcess::NotRunning; ++i) {
        m_process.waitForFinished(500);
    }
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(5000);
    }
    qInfo().noquote() << QStringLiteral("[STOP] done state=%1").arg(int(m_process.state()));
}
