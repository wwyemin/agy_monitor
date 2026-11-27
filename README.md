# ESP-Mosaico `agy_monitor` — Google Antigravity CLI 状态同步显示器

在 ESP-Mosaico 480×480 屏幕上以**黑底白字**高对比仪表盘实时显示 Mac 上 Google Antigravity CLI (`agy`) 的状态，支持 **USB 有线优先 + 脱离 USB 自动切换 Wi-Fi 同步**，并在屏幕顶部右上角与底部同步栏显示当前传输链路指示：

- **顶部链路指示徽章**：
  - **`USB`**（白底黑字实心胶囊）：已连接 USB，优先通过 ESP-Iris 有线高速同步
  - **`WiFi`**（黑底白框胶囊）：已脱离 USB，自动切换为局域网 Wi-Fi 同步
  - **`--`**（灰字暗框胶囊）：当前离线 / 等待 Mac 伴侣脚本响应
- **同步当前时间**：下发 Mac 本机本地时间（`HH:MM:SS` + `MM-DD 周X`），设备端每秒顺滑走时
- **剩余使用量**：配额百分比、周期、重置倒计时与进度条
- **当前 Model 与使用状态**：模型名、Agent 状态（`RUNNING` / `THINKING` / `IDLE` / `WAITING` / `ERROR` / `OFFLINE`）、上下文占用率、活跃会话数与当前工作区
- **自动更新 + 手动刷新**：默认每 **10 秒** 自动同步；点击屏幕左下角 **「手动刷新」** 或短按机身 **AI 键（GPIO7）** 均可立即向 Mac 发起秒刷（USB 与 Wi-Fi 模式均支持）

---

## 1. 编译与烧录到 ESP-Mosaico

在 ESP-Mosaico 工作区根目录下执行：

```bash
# 编译固件与 GSP UI 包
python3 mosaico.py project build --project projects/agy_monitor

# 通过 USB (ESP-Iris) 烧录到设备
python3 mosaico.py iris system-update --project projects/agy_monitor
```

如需在本地用模拟器预览：

```bash
python3 mosaico.py project sim --project projects/agy_monitor --interactive
```

---

## 2. 配置 Wi-Fi（脱离 USB 时自动切换用）

设备会直接读取 **Vibe Mode 共享 Wi-Fi 凭据**（无需单独写死在代码里）：

- 在设备开机按住 AI 键进入 Vibe Mode 的设置页面连上一次 Wi-Fi，或通过命令行配置（密码在交互提示中输入）：
  ```bash
  python3 mosaico.py iris test recovery-wifi --project projects/agy_monitor --ssid "<你的WiFi名称>"
  ```
- 插上 USB 同步过一次后，设备会自动记住 Mac 的局域网地址并存入 NVS；即使直接开机在 Wi-Fi 下使用，也会通过 UDP `18766` 端口自动发现同局域网的 Mac 伴侣脚本。

---

## 3. 配置 Antigravity CLI (`agy`) 实时状态桥接

进入一次 `agy` TUI 对话，执行官方 `/statusline` 命令挂载桥接脚本：

```text
/statusline <你的本地路径>/projects/agy_monitor/tools/mac_sync/agy_statusline_bridge.py
```

`agy` 会在每次模型/上下文/状态/配额变化时通过官方 JSON hook 写入 `~/.cache/agy-mosaico/status.json`，同时保留终端底部状态栏显示。

---

## 4. 启动 Mac 同步守护脚本（同时托管 USB + Wi-Fi）

直接运行即可同时开启 USB 同步与局域网 Wi-Fi HTTP/UDP 服务：

```bash
python3 projects/agy_monitor/tools/mac_sync/agy_mosaico_sync.py
```

常用参数：

- `--interval 10`：设置自动同步周期（秒，默认 `10`）
- `--wifi-only`：仅开启 Wi-Fi 局域网同步服务（不轮询 USB）
- `--usb-only`：仅开启 USB 同步（不监听 HTTP/UDP 端口）
- `--demo`：提供一组演示数据，快速验证界面与双模切换
- `--inspect`：查看 `~/.cache/agy-mosaico/status.json` 原始与归一化内容
- `--dry-run`：仅打印下发 JSON，不连接硬件或开启端口

---

## 5. IDEA 社区 `.irisfw` 固件包与目录结构

- **预编译 `.irisfw` 固件包**：`publish/agy_monitor-1.0.0.irisfw`（本地打包输出，不随源码提交；可直接上传至 ESP-Mosaico IDEA 社区）。
- **主要目录**：
  - `main/`：ESP32-S31 应用入口、ESP-Iris 自定义 RPC (`0x4147`) 注册、AI 按键与震动反馈
  - `components/app_agy_ui/`：480×480 黑底白字 GSP 界面、JSON 协议解析、纯整数时钟及 PC 模拟器适配器
  - `components/app_agy_net/`：USB 优先判断、Wi-Fi STA 自动重连、UDP 发现与 HTTP 拉取
  - `components/app_display/`：CO5300 显示与触摸板初始化
  - `tools/mac_sync/`：Mac 端 `/statusline` 桥接、USB/Wi-Fi 双模同步守护脚本与单元测试
  - `tests/`：纯 C 协议解析与时钟单元测试
