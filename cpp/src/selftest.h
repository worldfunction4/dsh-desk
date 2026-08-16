// --selftest：内嵌回归断言（对应 M1 的 AC1/AC3/AC4/AC5/AC6）。
//
// 纪律（CLAUDE.md §8.3 / docs/testing.md）：全部使用真实 `dsh web` 子进程，
// 绝不 mock；实例用隔离的临时 DSH_HOME，不污染用户配置；哨兵 key 只存在于
// 进程环境变量，绝不落盘或打印。
#pragma once

#include <QString>

class QCoreApplication;

// 运行全部自测；返回 0 表示全部通过，非 0 表示有失败。
int runSelfTest(QCoreApplication &app, const QString &dshRoot, const QString &launcher);