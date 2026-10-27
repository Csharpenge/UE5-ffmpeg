# UE5-ffmpeg

Unreal Engine **5.8.2** 屏幕录制 + RTMP 推流插件（`Plugins/UFFmpeg`）。

- 录制游戏视口到本地文件（.mp4/.flv/.mkv）或推流到 `rtmp://`
- 已升级到 **FFmpeg 9.0.2 API**，同时兼容 **4.x / 6.x / 8.x / 9.x** 系统库
- 失败不崩溃：初始化/编码失败返回 `nullptr` 并记录可读错误

## 环境要求

- Unreal Engine 5.8.2
- Windows：Visual Studio 2022/2026 + Windows SDK（UE 自带工具链即可）
- Android：NDK r27+（UE 5.8 自带 Android 平台组件）
- FFmpeg：
  - **Win64 / Linux / LinuxArm64 / macOS**：使用**系统安装的 FFmpeg 开发库**
  - **Android**：使用仓库内 `Plugins/UFFmpeg/ThirdParty/ffmpeg`（已内置 libx264 静态库，约 34 MB，不含调试符号）

## 平台支持

| 平台 | FFmpeg 来源 | 状态 |
|---|---|---|
| Win64 | 系统（vcpkg 等） | ✅ Editor/Game 编译、录制、`h264_mf` 编码实测通过 |
| Linux amd64 | 系统（apt） | ⚠️ 代码已适配，需在 Linux 主机上编译验证 |
| LinuxArm64 | 系统（交叉 sysroot） | ⚠️ 同上，需 aarch64 交叉编译环境 |
| Android arm64 | 仓库自带 9.0.2 + libx264 | ✅ 编译、链接、APK 打包通过（真机录制未验证） |
| macOS | 系统（brew） | ⚠️ 未测试 |

## 蓝图 API

### Create FFmpeg Director

| 参数 | 说明 |
|---|---|
| `World` | 世界上下文 |
| `VideoLength` | 录制时长；默认按**秒**（`bVideoLengthInSeconds=true`） |
| `OutFileName` | 输出文件的绝对路径，或 `rtmp://` 地址 |
| `VideoFilter` | **格式：`scale=WIDTH:HEIGHT`**（如 `scale=1920:1080`）；**留空 = 使用当前视口分辨率**；非法格式会回退视口分辨率并告警 |
| `UseGPU` | 优先硬件编码（`h264_nvenc → qsv → amf → mf`），失败自动回退软件（`libx264 → openh264 → mf`） |
| `FPS` | 帧率（1–240） |
| `VideoBitRate` | 码率（bit/s，如 6000000 = 6 Mbps） |
| `AudioDelay` | 音频延迟（秒） |
| `SoundVolume` | 线性增益：`1.0` 原音量、`0.5` 为 50%，>8 会被限制并告警 |
| `bVideoLengthInSeconds` | true：`VideoLength` 按秒（默认）；false：按帧数 |
| `IsRtmp` | true：把 `OutFileName` 当 RTMP URL 并强制使用 FLV 封装；false（默认）时，`rtmp://` 开头仍会自动识别 |

- **返回值**：成功返回 `UFFmpegDirector*`，失败返回 `nullptr`。
- 失败原因用 **`GetLastError()`** 读取；绑定 `OnRecordFinish` 前请先判空。
- 其它蓝图节点：`IsInitialized()`、`GetVideoEncoderName()`、`GetFFmpegVersion()`。

### VideoFilter 说明

```
scale=1920:1080   → 输出 1920x1080
scale=1280:720    → 输出 1280x720
（留空）           → 输出 = 当前视口分辨率（宽高向下取偶）
```

宽高会被取整到偶数（H.264/YUV420P 要求）。滤镜链内部固定为 `scale=W:H,format=yuv420p`。

## 运行时行为

- **采集**：D3D11 直接锁后缓冲；D3D12/Vulkan 用 `ReadSurfaceData` 回读并重新打包，避免行宽/步幅不一致导致画面错位。
- **像素**：统一按 **BGRA** 解包为 BGR24，再经 sws/filter 转 YUV420P（修复过 Alpha/Red 位重叠导致的偏红）。
- **编码器**：MediaFoundation 的 `h264_mf` 要求 COM MTA，插件在临时 MTA 线程完成 open/close，编码线程也初始化为 MTA。
- **时间戳**：帧 pts 按 `time_base` 正确换算并 `av_packet_rescale_ts`，音视频用 `av_interleaved_write_frame` 合流（解决旧的“录制加速”问题）。
- **失败保护**：所有 FFmpeg 调用都检查返回值，错误经 `av_strerror` 写入日志与 `GetLastError()`。
- **输出路径**：Windows 文件名的非法字符（`< > : " | ? *`）自动替换为 `_`；RTMP URL 不做处理。

## 版本兼容

- 代码通过 `Public/FFmpegCompat.h` 按头文件版本宏适配：
  - `AVChannelLayout/ch_layout`（≥5.1）
  - `swr_alloc_set_opts2`（≥5.1）
  - `av_opt_set_array`（8/9）与 `av_opt_set_int_list`（4/6）
  - `AV_PROFILE_*` 与 `FF_PROFILE_*`
- 运行时 DLL 名由编译期版本自动生成（如 `avcodec-62.dll`），并会校验运行时/头文件大版本是否一致。

## 已知限制

- 系统 FFmpeg 的 DLL **不会**被 UE 自动 stage 到发布包，发行时需随包分发或保证目标机安装同版本。
- RTMP 需要 FFmpeg 构建时开启 networking（当前系统的 8.0.1 已确认包含 `rtmp`/`tcp`）。
- 采集分辨率是视口偶数尺寸（不做 32 对齐），需要特定分辨率请用 `VideoFilter`。
- 仓库里保留的旧 FFmpeg 4.0 库（`ThirdParty/ffmpeg/lib/vs` 等）仅用于兼容性验证，不参与构建。
- **Android 自带的 FFmpeg 以 `--enable-gpl --enable-libx264 --disable-debug` 构建**（GPLv2+），对外分发 APK 时需遵守 GPL 条款。

# 引用
[原项目README](./README_old.md)
