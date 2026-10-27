# UFFmpeg 的 FFmpeg 链接策略

> 更新：2026-10-26

## 规则

| 平台 | 使用的 FFmpeg |
|---|---|
| **Android** | 仓库自带 `ThirdParty/ffmpeg`（`include/` + `lib/Android/<abi>/libav*.a`） |
| **Win64 / Linux / LinuxArm64 / Mac** | **系统环境安装的 FFmpeg**（开发库：头文件 + 链接库） |

Android 是唯一需要 `ThirdParty` 二进制的平台；其它平台不再需要往仓库里放库，也不参与仓库体积。

## 系统 FFmpeg 的查找顺序（`UFFmpeg.Build.cs`）

1. 环境变量 `UFFMPEG_SYSTEM_FFMPEG_ROOT`（前缀下必须有 `include/libavcodec/avcodec.h`）
2. 平台默认：
   - **Windows**：`%VCPKG_ROOT%\installed\x64-windows`（也接受 `VCPKG_INSTALLATION_ROOT`，再退到 `C:\vcpkg`、`C:\WorkTool\vcpkg`）
   - **Linux / LinuxArm64**：`/usr/local`、`/usr`
   - **macOS**：`/opt/homebrew`、`/usr/local`
3. 都找不到 → 构建直接报错，并打印各平台安装命令。

## 各平台准备

```powershell
# Windows（vcpkg，需含 avcodec/avformat/avutil/avfilter/swscale/swresample）
vcpkg install ffmpeg[core,avcodec,avformat,avfilter,swscale,swresample]:x64-windows
```

```bash
# Linux（Debian/Ubuntu）
sudo apt install libavcodec-dev libavformat-dev libavutil-dev libavfilter-dev libswscale-dev libswresample-dev
# Linux arm64 交叉：把同样的 -dev 包装进交叉 sysroot，或用 UFFMPEG_SYSTEM_FFMPEG_ROOT 指向含 include/lib 的 sysroot 前缀
# macOS
brew install ffmpeg
```

## 版本支持

构建时从所选头文件读取 `LIBAVCODEC_VERSION_MAJOR` 等，自动适配 **4.x / 6.x / 8.x / 9.x**：

- Windows DLL/导入库名按版本生成（`avcodec-62.dll` / `avcodec.lib`）；
- 代码差异由 `Public/FFmpegCompat.h` 的 `UFFMPEG_HAVE_*` 宏处理（ch_layout、swr_alloc_set_opts2、av_opt_set_array、AV_PROFILE_*）。

## 运行时与打包

- Windows：启动时优先从编译期写入的 `UFFMPEG_SYSTEM_ROOT\bin` 加载 DLL；运行期可用 `UFFMPEG_SYSTEM_FFMPEG_ROOT` 覆盖。
- 打包时**系统 DLL 不会被 UE 自动 stage**：要么随包分发 DLL，要么保证目标机已安装同样版本。
- Linux：链接系统 `libav*.so`（或自定义前缀下的 `.so/.a`）；Android：静态链接自带库。

## Android 需要准备的文件

```
Plugins/UFFmpeg/ThirdParty/ffmpeg/
├── include/                      # 已在仓库（9.0.2 头文件）
└── lib/Android/arm64-v8a/libavfilter.a libavformat.a libavcodec.a libswresample.a libswscale.a libavutil.a
```

静态库应开启 network（RTMP）并包含至少一个 H.264 编码器（`libx264` 或 `h264_mediacodec` 视需求）。
