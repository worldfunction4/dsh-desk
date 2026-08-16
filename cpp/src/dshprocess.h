// dsh-desk 桌面壳：`dsh web` 子进程管理（Qt/C++）。
//
// 官方事实依据（本地 checkout，与 Python 版 child.py 同一组契约）：
// - `dsh web` 是 `dsh --profile web` 的硬编码别名 —— apps/cli/src/args.ts
// - CLI 参数面：--host / --port / --trusted-host / -h,--help —— web-app/src/startup.ts
// - 默认 host 127.0.0.1 / port 3080 —— web-app/cordis.patch.yml
// - `--host 0.0.0.0` 被官方拒绝（暴露 RCE）—— web-app/src/startup.ts
// - stdout 的 `dsh web: http://...` 行是官方就绪信号（Loader settle 后才打印）
//   —— web-app/src/index.ts
//
// 敏感数据纪律（CLAUDE.md §5.3）：只继承父进程环境（方案 A），从不读取、
// 打印或写入任何 API key 文件；子进程输出仅保留于内存环形缓冲，不落盘。
#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

class QNetworkReply;

class DshProcess : public QObject {
    Q_OBJECT

public:
    // 就绪信号的来源：官方 URL 行（主）或 HTTP 200 兜底。
    enum class ReadySignal { None, UrlLine, Http200 };
    // 失败原因：就绪前进程退出，或等待超时。
    enum class FailReason { None, ProcessExited, Timeout };

    explicit DshProcess(const QString &dshRoot,
                        const QString &host = QStringLiteral("127.0.0.1"),
                        quint16 port = 0,          // 0 = 由操作系统分配空闲端口（官方契约）
                        int timeoutMs = 180000,    // 实测冷启动 39–43s，180s 富余
                        const QString &launcher = QString(),
                        QObject *parent = nullptr);
    ~DshProcess() override;

    // 官方红线防线：仅回环地址可绑定（构造即拒绝，spawn 前拦截）。
    static bool isLoopbackHost(const QString &host);

    // CLI 输入校验（notes/06 R7：非法值曾静默转 0）。
    // --port：0（OS 分配，官方契约）或 1–65535；非法返回 false 并在 *err 说明。
    static bool parsePortArg(const QString &raw, quint16 *out, QString *err);
    // --timeout：正整数毫秒（>0）；非法返回 false 并在 *err 说明。
    static bool parseTimeoutArg(const QString &raw, int *out, QString *err);

    void setExtraEnv(const QProcessEnvironment &env);  // 仅测试注入（哨兵 key / DSH_HOME）
    void start();     // spawn `dsh web --host 127.0.0.1 --port <0 或指定端口>`
    void stop(int waitMs = 15000);                     // taskkill /T /F 清整棵进程树

    QString url() const { return m_url; }
    // OS 实际绑定的端口（--port 0 时由官方 URL 行解析；显式端口时同 port()）。
    quint16 boundPort() const { return m_boundPort; }
    ReadySignal readySignal() const { return m_readySignal; }
    FailReason failReason() const { return m_failReason; }
    QString errorString() const { return m_error; }
    int exitCode() const { return m_process.exitCode(); }
    QStringList logLines() const { return m_log; }     // 内存环形缓冲（≤200 行），不落盘
    qint64 pid() const { return m_process.processId(); }
    quint16 port() const { return m_port; }
    QString host() const { return m_host; }
    QString dshRoot() const { return m_dshRoot; }
    bool finished() const { return m_finished; }

signals:
    void ready(const QString &url);                       // 就绪（url 为官方本地地址）
    void failed(DshProcess::FailReason reason, const QString &message);

private slots:
    void onStdout();
    void onProcessFinished(int code, QProcess::ExitStatus status);
    void onHttpPoll();
    void onHttpReplyFinished();
    void onTimeout();

private:
    void markReady(ReadySignal signal);
    void markFailed(FailReason reason, const QString &message);
    QString resolveLauncher() const;

    // 启动命令：program + 参数前缀（绕过 cmd 包装层，见 buildLaunchSpec 注释）。
    struct LaunchSpec {
        QString program;
        QStringList prefix;
    };
    LaunchSpec buildLaunchSpec() const;

    QProcess m_process;
    QTimer m_httpTimer;      // HTTP 200 兜底轮询
    QTimer m_timeoutTimer;   // 就绪超时
    QNetworkAccessManager m_nam;
    QNetworkReply *m_reply = nullptr;

    QString m_dshRoot;
    QString m_host;
    QString m_launcher;
    quint16 m_port;
    quint16 m_boundPort = 0;   // OS 实际绑定端口（--port 0 时由官方 URL 行解析）
    int m_timeoutMs;
    QProcessEnvironment m_extraEnv;

    QString m_url;
    ReadySignal m_readySignal = ReadySignal::None;
    FailReason m_failReason = FailReason::None;
    QString m_error;
    QStringList m_log;
    QString m_pendingLine;   // 分块读取时的残行
    int m_httpStreak = 0;    // HTTP 200 连续命中计数（≥3 判定）
    bool m_finished = false;

#ifdef Q_OS_WIN
    // kill-on-close Job：壳无论正常退出还是崩溃，Windows 都会终结整棵
    // 进程树（防孤儿的终极保证；句柄关闭即触发）。
    HANDLE m_job = nullptr;
#endif
};
