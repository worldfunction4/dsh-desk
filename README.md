# dsh-desk — DeepSeek Harness 桌面壳

把 DeepSeek Harness（dsh）的 `dsh web` 界面装进 Windows 桌面窗口的极薄 Qt 壳（C++20 / Qt 6 / QtWebEngine）。

壳只做四件事：

1. **启动**真实的 `dsh web` 子进程（仅绑定 `127.0.0.1`，绝不对外暴露）；
2. **等待**官方就绪信号（stdout 的 `dsh web: http://...` 输出行，HTTP 200 连续命中兜底）；
3. **加载**本地地址到 QtWebEngine —— **UI 零加工**，窗口内就是 dsh 原生界面，与浏览器打开完全一致；
4. **清理**：退出时终结整棵进程树，不留孤儿进程。

## AI 协作声明

本项目由开发者与 AI 编程助手（agent）协作完成：需求、架构决策与真机验收由人类主导，代码实现、测试与文档由 AI 辅助编写。所有功能均经过真机验证，技术结论以官方文档为准。

## 特性

- **双击即用**：自动拉起 `dsh web`、等待就绪、加载界面，无需手动开终端敲命令
- **端口零冲突**：默认 `--port 0`，由操作系统分配空闲端口（官方契约 "pass 0 to let the OS pick a free one"），退出即回收
- **干净退出**：Job Object（kill-on-close）+ `taskkill /T /F` + 进程 kill 三道防线，壳崩溃也不留孤儿
- **密钥零接触**：壳不读、不写、不打印任何 API key —— 把 `DEEPSEEK_API_KEY` 配成系统环境变量即可，子进程原样继承
- **渲染可调**：`--render auto|software|hardware`，可适配 GPU 驱动有问题的机器
- **内嵌自测**：`--selftest` 对真实 `dsh web` 子进程跑 36 项回归断言

## 环境要求

| 依赖 | 版本 | 说明 |
|------|------|------|
| Windows | 10 / 11 x64 | 开发验证平台 |
| Visual Studio 2022 | 17.x（MSVC 14.44） | 工作负载：**使用 C++ 的桌面开发** |
| CMake | ≥ 3.21 | VS 自带或独立安装 |
| Qt | **6.11.x msvc2022_64** | 在线安装器勾选 **Qt WebEngine** 组件；默认安装于 `C:\Qt\6.11.1\msvc2022_64` |
| Node.js | ≥ 22.19 | `dsh web` 的运行时 |
| dsh | checkout 源码（开发基线 0.1.0-rc.5） | 需含已构建的 `apps/cli/lib/bin.js`（见下） |


## 快速开始

### 1. 构建

```powershell
git clone https://github.com/worldfunction4/dsh-desk.git
cd dsh-desk

# Qt 不在默认路径时，把 C:/Qt/6.11.1/msvc2022_64 换成你的实际安装路径
cmake -S cpp -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/6.11.1/msvc2022_64
cmake --build build --config Release
```

产物：`build\Release\dsh-desk.exe`。

也可以直接用 VS Code（CMake Tools 插件；仓库自带 `cpp/.vscode/` 运行配置，支持 F5 调试）或 Qt Creator 打开 `cpp\CMakeLists.txt` 构建。

### 2. 运行

```powershell
# 让 exe 找到 Qt DLL（每次新开终端都需要）
$env:PATH = "C:\Qt\6.11.1\msvc2022_64\bin;$env:PATH"

# 告诉壳 dsh 在哪 —— 推荐设置环境变量，一劳永逸：
$env:DSH_DESK_DSH_ROOT = "D:\path\to\deepseek-harness"
# 也可以每次启动都带 --dsh-root（二选一即可）
build\Release\dsh-desk.exe --dsh-root D:\path\to\deepseek-harness
```

### 3. 验证安装（可选）

```powershell
build\Release\dsh-desk.exe --dsh-root D:\path\to\deepseek-harness --selftest
```

对真实 `dsh web` 子进程跑 36 项内嵌回归断言，全部 `PASS` 且退出码 0 即正常。

## 命令行参数

| 参数 | 默认 | 说明 |
|------|------|------|
| `--dsh-root <path>` | `$DSH_DESK_DSH_ROOT` | dsh 根目录（含 `package.json`），必填 |
| `--host <host>` | `127.0.0.1` | 绑定主机。**官方红线：禁止 `0.0.0.0`**（会被 dsh 拒绝） |
| `--port <port>` | `0` | `0` = OS 分配空闲端口；或 `1–65535` 显式指定（被占用会报错退出，不静默） |
| `--timeout <ms>` | `180000` | 就绪等待上限。直启构建产物实测约 9 秒；pnpm/tsx 回退链路约 100 秒 |
| `--launcher <path>` | 自动探测 | 覆盖 dsh 启动入口；默认直启 `apps/cli/lib/bin.js`，缺失时回退 pnpm |
| `--render <mode>` | `auto` | `auto` / `software`（强制软件渲染，GPU 异常导致闪烁/卡顿时用）/ `hardware`（强制 ANGLE D3D11） |
| `--selftest` | — | 运行内嵌回归断言后退出 |
| `--screenshot <path>` | — | 验收模式：渲染完成后截图并输出指标后退出 |
| `--edge-shot <path>` | — | 验收模式：壳退出前用 Edge headless 对同一 URL 截图（浏览器对照） |
| `--log-file <path>` | — | 日志同时写入文件（诊断用） |

环境变量：`DSH_DESK_DSH_ROOT`（等价 `--dsh-root`）；`QTWEBENGINE_CHROMIUM_FLAGS`（若你设置了它，壳完全尊重、不注入默认 Chromium flags）。

> VS Code F5 调试前，请先设置 `DSH_DESK_DSH_ROOT` 环境变量（`cpp/.vscode/launch.json` 已参数化为 `${env:DSH_DESK_DSH_ROOT}`）。

## 工作原理

```
[桌面壳 dsh-desk.exe]
  └─ spawn `node <dsh>/apps/cli/lib/bin.js web --host 127.0.0.1 --port <OS 分配>`
       （直启构建产物 ≈ npm 安装形态，实测就绪约 9s；lib 缺失时回退 pnpm/tsx 链路 ≈ 100s）
       └─ 就绪检测：官方 `dsh web: http://...` URL 行（主信号）+ HTTP 200 连续命中（兜底）
            └─ QWebEngineView 加载官方 URL —— 原生界面，零 UI 加工
                 └─ 退出清理三防线：Job Object(KILL_ON_JOB_CLOSE) → taskkill /T /F → QProcess::kill
```

实现要点（全部真机验证）：

- **绕过 cmd 包装层**：直接 spawn `node` 而不是 `pnpm.cmd` —— cmd 包装层会提前退出，导致进程树清理失去根节点、残留孤儿 node；
- **端口占用预检**：显式 `--port` 时先探测端口，避免 HTTP 兜底命中"别人的服务"；
- **终态幂等**：ready / failed 只触发一次；就绪前退出即判失败；
- **日志零落盘**：子进程输出只保存在内存环形缓冲（≤200 行），不写文件。

## 常见问题

| 现象 | 处理 |
|------|------|
| 启动报"找不到 dsh checkout" | 加 `--dsh-root` 或设置环境变量 `DSH_DESK_DSH_ROOT` |
| 启动后等很久（>30 秒） | 检查 dsh 是否已构建：`apps/cli/lib/bin.js` 存在则直启约 9 秒；缺失时回退 pnpm/tsx 链路约 100 秒 |
| 构建报找不到 Qt6WebEngine | Qt 安装时漏勾 **Qt WebEngine** 组件，用 MaintenanceTool 补装 |
| 显式 `--port` 报"端口已被占用" | 改用默认 `--port 0`（OS 自动分配，永不冲突） |
| 界面偶发闪烁、设置面板消失、拖拽卡顿 | GPU 驱动/虚拟显示器环境问题：先试 `--render hardware`；仍不行用 `--render software`（牺牲帧率换稳定）。壳默认已禁用 Windows 原生窗口遮挡计算与 backdrop-filter 毛玻璃（已知的闪烁/面板消失来源） |
| 设置了 `QTWEBENGINE_CHROMIUM_FLAGS` 但行为不像预期 | 壳检测到该变量时会完全尊重它、不注入默认 flags，请自行检查该变量内容 |
| 双击 exe 提示缺少 Qt DLL | 把 `C:\Qt\6.11.1\msvc2022_64\bin` 加入系统 PATH，或用 windeployqt 部署运行时（见下） |

## 部署（可选）

```powershell
C:\Qt\6.11.1\msvc2022_64\bin\windeployqt.exe --release --no-translations --compiler-runtime build\Release\dsh-desk.exe
```

目标机器仍需自行安装 Node.js 和 dsh。注意 QtWebEngine 是 LGPL-3.0：分发二进制时需遵循其 relink 条款（详见 Qt 文档）。

## 目录结构

```
dsh-desk/
├─ cpp/                    # C++/Qt 壳
│  ├─ src/                 # main（参数/渲染 flags）、DshProcess（子进程管理）、ShellWindow（窗口）、selftest
│  ├─ assets/              # 图标资源（dsh.png / dsh.ico）
│  ├─ CMakeLists.txt
│  ├─ CMakePresets.json    # VS 预设（msvc2022-release / msvc2022-debug）
│  └─ .vscode/             # VS Code 运行配置（F5 调试，路径已参数化）
├─ LICENSE                 # MIT
└─ README.md
```

## 许可证

[MIT](LICENSE) © worldfunction4
