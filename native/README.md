# RecDesk Native（Windows / C++20）

独立于 Electron 的 Windows 原生录屏项目，界面和录制控制使用 C++ / Win32，系统声音及麦克风使用 WASAPI，FFmpeg 作为独立编码进程。**不需要 Node.js、Electron 或 Qt。** 旧版源码保留在仓库根目录 `src/`，不会覆盖旧版用户数据。

## 运行

本机编译后的程序：`native/release/RecDesk.exe`。运行和分发时保留同目录的 `ffmpeg.exe` 及许可证文件。

1. 选择显示器、分辨率及帧率。默认 1080p / 30 FPS；小屏幕不放大。
2. 按需开启系统声音、麦克风和鼠标指针。
3. 点击“框选区域”可在桌面直接拖选，Esc / 右键取消；再次点击恢复整屏。
4. 点击“开始录制”，**先选择输出位置**，然后开始采集。
5. 点击主界面 / 悬浮窗的“停止”，或按 **Ctrl+Shift+F9**。停止后自动封装为 MP4。
6. 导出失败或取消时，点击“恢复录制 / 导入旧文件”重试。取消导出不等于删除录制。

分辨率预设按实际屏幕或区域生成：原始尺寸、2160p、1440p、1080p、720p、480p；输出保持比例，并对齐 H.264 所需偶数尺寸。支持 60 / 30 / 24 / 15 FPS、多显示器、负坐标和 Per-Monitor DPI。

## 为什么更适合长时间录制

```text
桌面 ─ gdigrab ─ H.264 ─┐
                       ├─ recording.mkv（持续落盘）─ 停止 ─ 流复制 ─ .partial.mp4 ─ 原子替换最终文件
系统音 + 麦克风 ─ WASAPI ─ 固定时间线 ─ 可取消命名管道 ─ AAC ─┘
```

- 不再经过浏览器 `MediaRecorder`、Canvas 绘制循环和大块 IPC 传输。
- **录制时完成视频编码**；原生录制导出使用 `-c copy`，不重新编码整段视频。
- 音频使用固定 2 秒时间线、10 ms 输出块和有限管道；按 QPC 时间戳放置音频，静音期间持续输出静音，避免等待音频导致画面无法启动。
- FFmpeg 视频 / 音频输入队列、诊断缓存及日志文件均有限制。编码积压或设备断开会停止并提示，不无限堆积数据。
- 日志和进度持续被后台线程读取，录制和导出不会阻塞界面消息循环。
- 录制 30 秒无输出、停止 15 秒未退出、导出 60 秒无进展时终止等待并保留源文件。上述是**无进展超时**，不是录制时长限制。
- 每 2 秒检查缓存磁盘空间；低于 256 MiB 停止。开始前检查至少 512 MiB 可用空间；导出前另外检查可容纳输出文件的空间。
- 先写唯一的临时 MP4，只有 FFmpeg 成功退出后才替换最终文件；失败不会覆盖已有输出。
- 不启用 `+faststart`，避免长视频在导出结尾再次整体搬移文件；本地播放不受影响，如需网页渐进播放可后续单独处理。

> C++ 本身不保证不丢帧。当前视频后端是兼容性优先的 **GDI / CPU x264**，不是 DXGI / GPU 硬编码。在 4K / 60 FPS、高负载或慢磁盘环境下应降低分辨率 / 帧率。当前不提供实时预览、暂停续录、按应用采音或运行中切换音频设备。

## 文件安全与恢复

会话目录：`%LOCALAPPDATA%\RecDeskNative\Sessions\<GUID>\`

| 文件 | 用途 |
| --- | --- |
| `recording.mkv` | 已编码的恢复源，成功导出后才删除 |
| `target.txt` | UTF-8 编码的预期输出位置 |
| `recording.log` | FFmpeg 录制日志和进度，最多约 8 MiB |
| `export.log` / `recovery.log` | 导出 / 恢复诊断 |

程序异常关闭时，Windows Job Object 会清理其编码子进程，源 MKV 留在磁盘。已完整写入的 MKV 数据通常可恢复；断电、坏盘或最后一块未写完的数据不保证恢复。正常关闭时会等待停止和导出，避免直接杀掉录制。

设置界面和悬浮窗**默认可见，也允许被远程桌面 / 屏幕共享显示**，避免出现运行后看不到参数的问题。只有显式运行 `RecDesk.exe --exclude-controls` 时，才在实际录制期间请求将控制窗口排除在屏幕采集之外；停止后设置窗口恢复普通显示。远程使用时不要开启该选项。旧的 `--allow-capture` 参数仍可使用，但不再是显示界面的必要条件。

导出期间需要源文件和目标 MP4 同时存在，**至少预留约两份录制文件的空间**；当前缓存位于系统盘。系统盘不足时请先释放空间，不要误以为选择其他输出盘会移动录制缓存。

旧版 Electron 的 `.webm` 可通过“导入旧版 WebM / MKV 文件”恢复，通常位于 `%TEMP%\screen-recording-*\recording.webm`，旧恢复标记位于 Electron 用户数据的 `recording-recovery` 中。旧 WebM 需要重新编码，耗时取决于视频长度；导入的源文件在成功或失败后都**不会自动删除**。已经被旧版删除、或缺少有效 WebM 头的文件不能保证恢复。

## 编译

要求：Windows 10/11 x64、Visual Studio 2022 或 2026 的“使用 C++ 的桌面开发”、Windows SDK、CMake 3.24+（VS 2026 需要支持对应生成器的新版本）。FFmpeg 必须包含 `gdigrab`、`libx264`、`aac`，导入旧 WebM 还需要相应解码器。

在仓库根目录运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File native\build.ps1

# 或显式指定 FFmpeg
powershell -NoProfile -ExecutionPolicy Bypass -File native\build.ps1 -Ffmpeg C:\tools\ffmpeg.exe

# 调试构建
powershell -NoProfile -ExecutionPolicy Bypass -File native\build.ps1 -Configuration Debug
```

脚本会自动寻找 PATH / Visual Studio 中的 CMake、PATH 或旧版 `node_modules/ffmpeg-static/ffmpeg.exe`，编译、运行 CTest，再输出到 `native/release/`。仅借用旧版已有二进制时也不需要运行 Node.js。

如果仍使用 npm 管理入口，也可执行 `npm run build:native` 和 `npm run test:native`。原有 `npm run dev` / `npm run build` 仍启动 / 构建 Electron 旧版。

手动构建：

```powershell
cmake -S native -B native/build -A x64 -DRECDESK_FFMPEG=C:/tools/ffmpeg.exe
cmake --build native/build --config Release --parallel
ctest --test-dir native/build -C Release --output-on-failure
cmake --install native/build --config Release --prefix native/release
```

## 验证

- 核心测试：Windows 参数引用、多屏负坐标、偶数尺寸、长时长格式、命令策略、进度解析、正常停止、强制停止。
- 导出集成测试：只生成合成媒体，不录制桌面 / 麦克风；检查 MP4 完整解码、中文 / 空格路径、失败与取消时保留源文件和已有输出、拒绝覆盖源文件、清理临时 MP4。找不到 FFmpeg 时该组会明确跳过。
- 界面回归测试：短暂打开设置窗口，不录屏或录音，检查默认参数控件可见、未启用屏幕采集排除、启动器要求隐藏时仍显示、窗口移出屏幕 / 最小化后重复启动可恢复；当前桌面已有运行实例时明确跳过该组。
- 真正的桌面 / 设备测试为**手动执行**，需要交互桌面、默认音频设备及麦克风权限：

```powershell
# 以下命令实际录制桌面及声音；请使用非敏感测试画面
native\build\Release\recdesk_smoke.exe --seconds 10 --audio none --output C:\Videos\silent.mp4
native\build\Release\recdesk_smoke.exe --seconds 60 --height 1080 --fps 30 --audio both --output C:\Videos\test.mp4
native\build\Release\recdesk_smoke.exe --seconds 5 --height 0 --region 100,100,801,451 --audio none --output C:\Videos\region.mp4
native\build\Release\recdesk_smoke.exe --recover C:\path\recording.mkv --output C:\Videos\recovered.mp4

# 单个测试组
ctest --test-dir native/build -C Release -R '^recdesk_core$' --output-on-failure
ctest --test-dir native/build -C Release -R '^recdesk_export$' --output-on-failure
ctest --test-dir native/build -C Release -R '^recdesk_ui$' --output-on-failure
```

`--audio` 支持 `none` / `system` / `mic` / `both`。自动化测试不能代替数小时录制、多显卡、多 DPI、睡眠唤醒和异常断电测试。

已执行的本机验证及限制记录见 `VALIDATION.md`。

## 源码结构

```text
native/
  CMakeLists.txt / build.ps1       编译、测试和本地发布
  src/main.cpp                    中文 Win32 主界面、悬浮窗、快捷键
  src/region_selector.*           物理像素桌面框选
  src/recorder.*                  会话状态、磁盘检查、恢复和安全导出
  src/audio.*                     WASAPI 捕获、定长混音时间线、可取消音频管道
  src/encoder_process.h           编码子进程接口（避免与 CRT process.h 重名）
  src/process.cpp                 Job Object、受限句柄继承、进度和日志读取
  src/platform.*                  Windows / COM / 路径 / 显示器辅助函数
  resources/app.manifest          DPI、长路径及普通用户权限
  tests/                          核心测试、合成媒体导出测试、手动桌面测试程序
```

许可证和 FFmpeg 分发注意事项见 `THIRD_PARTY_NOTICES.md`。
