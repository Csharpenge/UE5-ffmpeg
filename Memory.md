# Memory — UE5-ffmpeg 迁移到 Unreal Engine 5.8.2

> 最后更新：2026-10-26 17:28 (+08:00)
> 工作区：`F:\Project\UE5-ffmpeg`　引擎：`D:\Epic\UE_5.8`（5.8.2，CL 56702186，`++UE5+Release-5.8`）

---

## 1. 目标

将原本面向 **UE 5.0.2** 的工程（录屏 + RTMP 推流插件 UFFmpeg）适配到 **UE 5.8.2**，编译通过并检查问题。

测试平台范围（用户明确指定，其它平台不测）：**Win64**、**Linux amd64**、**Android**。

---

## 2. 当前状态总览

| 平台 / 目标 | 状态 | 产物 |
|---|---|---|
| Win64 Editor (Development) | ✅ 成功，0 warning | `Binaries/Win64/UnrealEditor-ffmpeg.dll`、`Plugins/UFFmpeg/Binaries/Win64/UnrealEditor-UFFmpeg.dll` |
| Win64 Game (Development) | ✅ 成功 | `Binaries/Win64/ffmpeg.exe` |
| Win64 Game (Shipping) | ✅ 成功 | `Binaries/Win64/ffmpeg-Win64-Shipping.exe` |
| Android arm64 (Development) | ❌ 链接失败（缺 Android ffmpeg 库） | 源码 100% 编译通过 |
| Linux amd64 (Development) | ⛔ 无法开始（引擎缺 Linux 组件） | — |

运行时验证：`UnrealEditor-Cmd` 启动工程成功，日志确认插件模块加载：

```
LogModuleManager: InternalLoadLibrary: 'UFFmpeg' (F:/Project/UE5-ffmpeg/Plugins/UFFmpeg/Binaries/Win64/UnrealEditor-UFFmpeg.dll)
```

7 个 ffmpeg DLL（avutil/swresample/avcodec/avformat/swscale/postproc/avfilter）加载正常。
（该次验证发生在"移除 avdevice"之前；移除后只做了编译验证，**运行期复验待办**。）

---

## 3. 环境事实（重要，避免重复排查）

- 引擎：`D:\Epic\UE_5.8` = 5.8.2；另有 `D:\Epic\UE_5.7`(5.7.4)、`D:\Epic\UE_4.27`、`D:\Epic\UE_5.1`（5.1 不完整，无 Build.version）。
- VS：`C:\Program Files\Microsoft Visual Studio\18\Community`，MSVC 14.50.35739 / 工具链 14.50.35717。
- Windows SDK：10.0.22621.0（另有 10.0.26100.0）。
- Android：NDK `C:\Users\lihk6\AppData\Local\Android\Sdk\ndk\27.2.12479018`（r27c，clang 18.0.3，`android-29`），UBT 校验 **VALID**。
- Linux：**无交叉编译组件**，UBT 校验 `INVALID v26_clang-20.1.8-rockylinux8`，需在 Epic Launcher 勾选 Linux 平台组件。
- ffmpeg 第三方库：**4.0**（avcodec 58.18.100 / avformat 58 / avutil 56 / swscale 5 / swresample 3 / avfilter 7 / postproc 55）。
  - Win64：`ThirdParty/ffmpeg/lib/vs/x64/*.lib` + `bin/vs/x64/*.dll`
  - Linux：`ThirdParty/ffmpeg/lib/amd64/*.a` + `include/amd64`
  - **Android：没有任何库**
- UBT 编译设置基线：V7 + `Unreal5_8` include order。

### 文件系统 / 沙箱注意（会反复遇到）

- 会话默认是 `workspace-write` 沙箱；**项目里已存在的子目录**（`Source`、`Plugins`、`Intermediate`、`Binaries`、`Saved`、`Content`、`.vs`）**没有**沙箱写入权限（只有工作区根目录和新建目录继承到了 DSH 的写入授权）。
- 表现：`[sandbox: file access denied under workspace-write mode]`，即使只是 `New-Item Intermediate\x`。
- 解决：把会话切到「完全权限」，或对单条命令使用一次性升级（`danger-full-access`）。
- 会话开始时的权限修复记录：`F:\Project\dsh-acl-recovery\`（备份 + 回滚脚本）。
  回滚命令：
  ```
  pwsh -NoProfile -File 'F:\Project\dsh-acl-recovery\acl-backup-2c273bb6c50945a19bb291343b27cd85.json.ps1' -Path 'F:\Project\UE5-ffmpeg' -AllowRoot 'F:\Project\UE5-ffmpeg' -Restore 'F:\Project\dsh-acl-recovery\acl-backup-2c273bb6c50945a19bb291343b27cd85.json'
  ```

---

## 4. 已完成修改（8 个文件）

### 4.1 构建配置

- `Source/ffmpeg.Target.cs`、`Source/ffmpegEditor.Target.cs`
  - `BuildSettingsVersion.V2` → **`V7`**（必须；否则 UBT 报：
    `... has build products in common with UnrealEditor ... This is not allowed`）
  - 新增 `IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;`

### 4.2 插件 API 迁移

- `Plugins/UFFmpeg/Source/UFFmpeg/Public/FFmpegDirector.h`
  - 新增 `#include "ISubmixBufferListener.h"`（5.8 不再间接包含，原报 `base class undefined`）
  - `UFFmpegDirector` **不再**继承 `ISubmixBufferListener`（5.4+ 它是 `TSharedFromThis`，UObject 无法满足）
  - 新增适配器类 `FFFmpegSubmixListener`（成员 `TSharedPtr<..., ESPMode::ThreadSafe> SubmixListener`、`TWeakObjectPtr<USoundSubmix> RegisteredSubmix`）
  - `GameTexture`：`FTexture2DRHIRef` → **`FTextureRHIRef`**（5.8 已删除 `FTexture2DRHIRef`）
  - `OnBackBufferReady_RenderThread(SWindow&, ISlateViewportProvider&)`
- `Plugins/UFFmpeg/Source/UFFmpeg/Private/FFmpegDirector.cpp`
  - 音频注册改为 `RegisterSubmixBufferListener(SubmixListener.ToSharedRef(), AudioDevice->GetMainSubmixObject())`，注销同理
    ⚠️ 5.8 中旧的裸指针重载是"只打错误日志"的空实现 → 不迁移会**静默采不到音频**
  - 后缓冲改为 `ViewportProvider.GetBackBufferResource()`（委托签名已变），并加空指针保护
  - 纹理读取：`GameTexture.GetReference()`（`TRefCountPtr` 没有 `.Get()`）
  - 删除死代码 `AddEndFunction` / `EndWindowReader` / `EndWindowReader_StandardGame`（`OnSlateWindowDestroyed` 已移除）
  - `qcompress = 0.8f`（消除 C4305）
  - 文件由 **GBK 转 UTF-8**，中文注释改为 ASCII 英文
- `Plugins/UFFmpeg/Source/UFFmpeg/Public/EncodeData.h`
  - 同样 GBK → UTF-8，注释改英文
- `Plugins/UFFmpeg/Source/UFFmpeg/Private/UFFmpeg.cpp`、`Public/UFFmpeg.h`
  - `PLATFORM_64BITS` 弃用 → 固定 x64 路径
  - 移除未使用的 `avdevice`（其 Windows DLL 依赖 `OpenAL32.dll`/`AVICAP32.dll`，加载报 error 126）

### 4.3 构建依赖

- `Plugins/UFFmpeg/Source/UFFmpeg/UFFmpeg.Build.cs`
  - 移出 `avdevice.lib`、`avdevice-58.dll`（delay-load / RuntimeDependencies）

---

## 5. 构建命令

```powershell
# Editor
& 'D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat' ffmpegEditor Win64 Development "-Project=F:\Project\UE5-ffmpeg\ffmpeg.uproject" -WaitMutex

# Game Development / Shipping
& 'D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat' ffmpeg Win64 Development "-Project=F:\Project\UE5-ffmpeg\ffmpeg.uproject" -WaitMutex
& 'D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat' ffmpeg Win64 Shipping   "-Project=F:\Project\UE5-ffmpeg\ffmpeg.uproject" -WaitMutex

# Android / Linux（当前均失败，见待办）
& 'D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat' ffmpeg Android Development "-Project=F:\Project\UE5-ffmpeg\ffmpeg.uproject" -WaitMutex
& 'D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat' ffmpeg Linux   Development "-Project=F:\Project\UE5-ffmpeg\ffmpeg.uproject" -WaitMutex
```

编译日志（`Saved/Logs/`）：`Build_5.8_6.log`（Editor 成功）、`Build_5.8_game_dev2.log`、`Build_5.8_game_ship2.log`、`Build_5.8_android.log`（Android 链接错误）、`ffmpeg.log`（编辑器启动，含插件加载）。

---

## 6. 待办（按优先级）

### P0 — Android 编不过

- [ ] 提供 Android arm64-v8a 的 ffmpeg 4.0 静态库（`libavcodec.a`、`libavformat.a`、`libavutil.a`、`libavfilter.a`、`libswscale.a`、`libswresample.a`、`libpostproc.a`、`libavdevice.a`），放到 `Plugins/UFFmpeg/ThirdParty/ffmpeg/lib/android/arm64-v8a/`
- [ ] `UFFmpeg.Build.cs` 增加 Android 分支（`PublicAdditionalLibraries` + include 路径）
- [ ] 确认 Android 上 `UFFmpeg.cpp::LoadLibrary` 走静态链接路径（非 Windows/Mac 时返回 nullptr 即可）
- [ ] 复查 `GetScreenVideoData` 在 Android/Vulkan 下 `RHIName == "Vulkan"` 分支是否可用（`ReadSurfaceData` 对移动端后缓冲的可行性待验证）
- 现状错误：`ld.lld: error: undefined symbol: av_write_trailer ...`（`-Wl,--no-undefined`，缺一个符号都链不过）
- 备选方案（需产品决策）：若不打算在 Android 提供录屏，则用平台宏把 ffmpeg 相关实现排除/桩化，让 Android 能打包

### P0 — Linux 无法验证

- [ ] 在 Epic Games Launcher 勾选 Linux 平台组件（`v26_clang-20.1.8-rockylinux8`），然后重跑 Linux 构建
- [ ] 插件侧已具备 `lib/amd64/*.a` 与 `include/amd64`，预计可直接链接；未验证

### P1 — 运行期内存泄漏（已定位，未修改）

- [ ] `Encode_Video_Frame`：`filt_frame` 每帧 `av_frame_alloc`，只 `av_frame_unref` 未 `av_frame_free` → 泄漏
- [ ] `Encode_Video_Frame` / `Encode_Audio_Frame`：`video_pkt` / `audio_pkt` 每帧 `av_packet_alloc`，只 `unref` 未 `av_packet_free` → 泄漏
- [ ] `GetScreenVideoData` 的 D3D12/Vulkan 路径：`FMemory::Malloc(4*W*H)` 的 `TextureData` 入队后未释放 → 每帧泄漏（队列是 memcpy 拷贝）
- [ ] `Encode_Finish`：`avcodec_free_context(&ctx)` 之后又 `avcodec_close(ctx)` / `av_free(ctx)`（空指针，冗余，不崩溃）→ 清理

### P2 — 文档 / 工程文件 / 复验

- [ ] 重新生成 VS 工程文件（`ffmpeg.sln` 等仍是 5.0 时代内容；`Automation_ffmpeg.sln(x)`、`ffmpeg.slnx`、`.vsconfig` 是未跟踪文件）
- [ ] 更新 `README.md`（仍写 "UE 5.0.3"），补充 5.8.2 适配说明与平台支持矩阵
- [ ] 移除 avdevice 后重跑一次编辑器启动 smoke test，确认日志中不再出现 `Failed to load avdevice-58.dll`
- [ ] README 提到的"录制视频加速（播放速度偏快）"问题仍未处理

---

## 7. 已知现象 / 备注

- 用 `-ExecCmds="quit"` 启动 `UnrealEditor-Cmd` 时，`quit` 命令执行后进程不会及时退出（会继续跑 HTTP/UDP/DDC 维护）；验证日志时需手动 `Stop-Process`。
- 项目根目录 17:24 出现了未跟踪的 `ffmpeg-9.0.2/`（ffmpeg 源码树），**不是本会话创建的**，未做任何改动。
- `ffmpeg.sln`、`ffmpeg.uproject` 在本会话开始前就已是 modified 状态（`.uproject` 的 `EngineAssociation` 已是 `5.8`）。
- 当前 git 工作区改动：`Plugins/UFFmpeg/Source/**`（6 个文件）、`Source/*.Target.cs`（2 个文件）；未提交。

---

## 8. 关键代码位置速查

| 内容 | 位置 |
|---|---|
| 音频监听注册/注销 | `FFmpegDirector.cpp` 的 `Begin_Receive_AudioData()` / `DestoryDirector()` / `Stop()` |
| 后缓冲回调 | `FFmpegDirector.cpp` 的 `OnBackBufferReady_RenderThread()` |
| 屏幕取帧（D3D11/D3D12/Vulkan） | `FFmpegDirector.cpp` 的 `GetScreenVideoData()` |
| 编码器初始化 | `Create_Video_Encoder()` / `Create_Audio_Encoder()` |
| 每帧编码与写包 | `Encode_Video_Frame()` / `Encode_Audio_Frame()` |
| 编码线程 & 环形队列 | `EncoderThread.cpp` / `EncodeData.cpp` |
| 模块启动加载 DLL | `UFFmpeg.cpp` 的 `StartupModule()` / `LoadLibrary()` |
| 第三方库链接 | `UFFmpeg.Build.cs` |

---

## 9. 运行时崩溃记录（2026-10-26 17:33）

**现象**：编辑器内调用 `CreateFFmpegDirector` → `Assertion failed: false [FFmpegDirector.cpp] [Line: 450]`。
调用栈：`Initialize_Director` → `Create_Video_Encoder` → `avcodec_open2` 失败 → `check(false)`。

**结论：与 5.8 迁移无关**，是第三方 **ffmpeg 4.0 的 NVENC 与新版 NVIDIA 驱动不兼容**（同样的 5.0 工程在这台机器上也会崩）。

**证据**（用独立探针程序直连同一套 `ThirdParty` DLL 复现，1920x1080@30 这种正常参数也失败）：

- 该 ffmpeg 构建参数：`--enable-nvenc --enable-cuda ...`，**没有 libx264 / 原生 h264 编码器**。
- 枚举 `AV_CODEC_ID_H264` 编码器只有 `h264_nvenc`；`avcodec_find_encoder(AV_CODEC_ID_H264)` 返回 `h264_nvenc`；`libx264`、`h264` 均不存在。
- `avcodec_open2` 返回 **-40 `AVERROR(ENOSYS)` "Function not implemented"**。
- ffmpeg 日志顺序：
  `Loaded Nvenc version 13.1` → `Nvenc initialized successfully` → `1 CUDA capable devices found` → `supports NVENC` → `Cannot get the preset configuration: unsupported param (12)` → `avcodec_open2 -> -40`。
- 机器：RTX 3070 / 驱动 **616.92**（NVENC API 13.1）。
- 反证：同一台机器用 **ffmpeg 8.1**（`C:\WorkTool\ffmpeg\bin\ffmpeg.exe`，带 libx264 + nvenc）测 `-c:v h264_nvenc` 和 `-c:v libx264` 均 `EXIT=0` → 显卡/驱动正常，问题只在旧的 ffmpeg 4.0。
- 原因：ffmpeg 4.0 用旧接口 `nvEncGetEncodePresetConfig` + 旧 preset GUID（NVENC SDK 8.1，2018 年）；新版驱动已移除 legacy preset 支持。ffmpeg 9.0.2 已改为 `nvEncGetEncodePresetConfigEx` + `NV_ENC_PRESET_P1..P7`。

**影响**：`UseGPU=true` 与 `false` **都会失败**——非 GPU 路径的 `avcodec_find_encoder(AV_CODEC_ID_H264)` 同样解析到 `h264_nvenc`（它是该构建里唯一的 H.264 编码器），所以必崩。

**修复方向（待办）**：

- [ ] 代码健壮性（可立即做）：编码器查找/`avcodec_open2` 失败时用 `av_strerror()` 记录可读原因并优雅返回，不再 `check(false)`；同时修 `out_video_stream` 先解引用后判空、`fps<=0`（time_base 非法）、高度非偶数、4K + level 3.0 等参数校验。
- [ ] 更换第三方 ffmpeg（根治）：优先选带 `libx264`（软件兜底）+ 新版 NVENC 的构建；注意 DLL 名（avcodec-61/62…）与 API 变化（`avcodec_close` 移除、`ch_layout`、`av_init_packet` 移除、`avcodec_parameters_from_context` 等），代码需要一并迁移。
- [ ] 编码器选择改为显式候选链：`h264_nvenc` → `h264_qsv` → `h264_amf` → `libx264` → `h264_mf`，并区分 `UseGPU` 语义。
- 参考：工作区已有未配置的 `ffmpeg-9.0.2/` 源码树（非本会话创建），可直接用于重编 Windows/Android/Linux 三平台库。
- 诊断探针（`probe.cpp` / `probe2.cpp`）用完已删除；需要时可按本文第 9 节参数重建。

---

## 10. FFmpeg 9.0.2 API 升级完成（2026-10-26 17:56）

目标平台：**Win64 / Linux amd64 / LinuxArm64 / Android**。第 9 节的 4.0 问题已通过整体升级到 9.0.2 解决。

### 10.1 已完成

- **头文件已替换为 FFmpeg 9.0.2**：`ThirdParty/ffmpeg/include/`（975 个头文件，9.5 MB），并补了 configure 才会生成的两个头：`libavutil/avconfig.h`、`libavutil/ffversion.h`。
- **代码迁移**（`FFmpegDirector.h/.cpp`）：
  | 旧 (4.0) | 新 (9.0.2) |
  |---|---|
  | `avcodec_close()` | 删除（只保留 `avcodec_free_context`） |
  | `ctx->channels` / `ctx->channel_layout` | `av_channel_layout_default(&ctx->ch_layout, 2)` |
  | `qmin/qmax/qcompress/max_qdiff/me_range/frame_number` | 全部删除（字段已移除） |
  | `FF_PROFILE_H264_BASELINE` | `AV_PROFILE_H264_BASELINE` |
  | `av_opt_set_int_list(...,"pix_fmts",...)` | `av_opt_set_array(..., AV_OPT_TYPE_PIXEL_FMT, ...)` |
  | `av_opt_set_int(swr,"in_channel_layout",...)` | `swr_alloc_set_opts2()` |
  | `av_image_alloc()` 裸缓冲帧 | `av_frame_get_buffer()`（可被 filtersrc 正确 ref） |
  | `av_write_frame()` | `av_interleaved_write_frame()` + `av_packet_rescale_ts()` |
  | `av_init_packet()` | 删除（已废弃） |
  | `swr_close()` | 删除 |
- **失败保护**：
  - 所有 `check(false)` 已清除（0 处），改为 `Fail()`：写 `LastError` + `UE_LOG(LogFFmpeg, Error)` + 优雅返回。
  - `Initialize_Director` 返回 **bool**；`CreateFFmpegDirector` 失败返回 **nullptr**，并可通过新加的 `UFFmpegFunctionLibrary.GetLastError()`（蓝图可调）拿到原因。
  - 新增 `UFFmpegDirector.IsInitialized()` / `GetLastError()` / `GetVideoEncoderName()`。
  - 参数校验：fps 1~240、分辨率 2~8192 且宽高取偶、bitrate 下限、空 World/GameViewport/窗口/文件名检查。
  - 每一步 FFmpeg 调用（alloc/open/new_stream/avio_open/write_header/filter/swr/sws）都检查返回值并用 `av_strerror` 记录。
  - 资源释放幂等（`FreeEncodingResources`），同时修掉了原来的每帧泄漏（filt_frame / packet / D3D12 TextureData）。
- **编码器选择**：显式候选链 + 逐级回退
  - GPU 优先：`h264_nvenc` → `h264_qsv` → `h264_amf` → `h264_mf`
  - 软件优先：`libx264` → `h264_openh264` → `libopenh264` → `h264_mf`
  - 最后兜底 `avcodec_find_encoder(AV_CODEC_ID_H264)`；实际使用的编码器写入 `VideoEncoderName`。
- **Build.cs 重写**（`ThirdParty` 新布局，见 `Plugins/UFFmpeg/ThirdParty/ffmpeg/BUILD_9.md`）：
  - Win64：`lib/Win64/*.lib` + `bin/Win64/*.dll`（avcodec-63 / avformat-63 / avutil-61 / avfilter-12 / swscale-10 / swresample-7），delay-load + RuntimeDependencies。
  - Linux / **LinuxArm64**（新增）：`lib/Linux`、`lib/LinuxArm64` 的 `.a`，附加 `-lm -lpthread -ldl`。
  - Android：`lib/Android/<abi>/`（arm64-v8a / x86_64 / armeabi-v7a 自动映射），附加 `-lm -lz -llog`。
  - 不再链接 `libpostproc`（9.x 已删除该库）与 `libavdevice`（未使用）。
  - 库缺失只打 WARNING，不阻塞编译（便于 `-NoLink` 验证）。
- **UFFmpeg.cpp**：DLL 版本号更新为 61/7/63/63/10/12，移除 postproc/avdevice 加载，插件查找失败/缺少 DLL 时有明确日志。

### 10.2 编译验证结果

| 目标 | 命令 | 结果 |
|---|---|---|
| Win64 Editor Development | `Build.bat ffmpegEditor Win64 Development ... -NoLink` | ✅ 0 error 0 warning |
| Win64 Game Development | `Build.bat ffmpeg Win64 Development ... -NoLink` | ✅ |
| Android arm64 Development | `Build.bat ffmpeg Android Development ... -NoLink` | ✅（clang 18 / NDK r27c） |
| Linux / LinuxArm64 | — | ⛔ 本机无 Linux SDK，未验证 |

日志：`Saved/Logs/Build_9x_nolink.log`、`Build_9x_android_nolink.log`。

### 10.3 尚未完成（阻塞运行）

- [ ] **FFmpeg 9.x 二进制库未提供** → 目前只能编译（`-NoLink`），**不能链接/运行**。按 `BUILD_9.md` 生成并放入对应目录即可。
- [ ] 链接通过后做一次完整 build + 运行验证（重点：编码器是否能打开、RTMP 是否能推）。
- [ ] Linux / LinuxArm64 需在 Epic Launcher 安装 Linux 平台组件后才能编译验证。
- [ ] 旧的 4.0 库仍在 `lib/vs`、`lib/amd64`、`lib/osx`、`bin/vs`（已不被引用，可删）。
- [ ] 已链接的旧二进制（`Plugins/UFFmpeg/Binaries/Win64/UnrealEditor-UFFmpeg.dll`）还是 4.0 时代产物，**在重新链接前不要在编辑器里调用录屏**，否则仍会复现第 9 节的崩溃。

### 10.4 给库的接口约定（避免再次踩坑）

- 构建 FFmpeg 时必须：**开启 network**（RTMP 需要）、**至少一个软件 H.264 编码器**（`--enable-libx264`，9.x 已无原生 h264 编码器）。
- 头文件版本必须与库一致（都是 9.0.2 / avcodec 63），否则 ABI 不匹配。
- Linux/Android 需要 `libx264` 与 FFmpeg 链接到同一个 C 运行时；静态库顺序已在 Build.cs 中按依赖排列（filter → format → codec → swresample → swscale → util）。

---

## 11. 构建工具链盘点（2026-10-26 18:05）

目标是用工作区 `ffmpeg-9.0.2/` 为 Win64 / Linux amd64 / LinuxArm64 / Android 编库。

### 11.1 现有可用

| 工具 | 位置 / 版本 | 用途 |
|---|---|---|
| MSVC | VS18 Community，14.38 / 14.44 / 14.50 / 14.51 | Win64 编译器 |
| Windows SDK | 10.0.22621 / 10.0.26100（`rc.exe` 在 SDK 里，✓） | Win64 资源/链接 |
| MSYS2 | `C:\WorkTool\msys2`（bash 5.3、sed、gawk、grep、coreutils、tar、xz、zstd、perl、curl/wget、cygpath、install） | POSIX 环境跑 configure |
| GNU make 4.3 | NDK：`...\ndk\27.2.12479018\prebuilt\windows-x86_64\bin\make.exe` | 可替代 MSYS2 缺失的 make |
| nasm 3.01 | `C:\WorkTool\vcpkg\downloads\nasm-3.01-win64.zip`（离线） | x86 asm（也可用 `--disable-x86asm` 跳过） |
| pkgconf 2.5.1 | `C:\WorkTool\vcpkg\downloads\msys2-*pkgconf*.pkg.tar.zst`（离线） | 探测外部库（如 x264） |
| Android NDK | r27c（clang 18，`aarch64-linux-android29-clang`），含 make | Android arm64 |
| vcpkg | `C:\WorkTool\vcpkg`，已装 FFmpeg **8.0.1**（Win64，仅 `h264_mf` 编码器） | 仅可作临时备选，**不是 9.0.2** |
| 其它 | git、cmake、ninja、python、磁盘 C 275GB / F 98GB | |

### 11.2 缺失 / 阻塞

1. **MSYS2 没有 make**（可用 NDK make 顶上）。
2. **MSYS2 没有 gcc/clang/windres**（Win64 走 MSVC 不需要；MinGW 路线才需要）。
3. **nasm 不在 PATH**（离线 zip 可解压）。
4. **pkg-config 不在 PATH**（离线 pkgconf 包可解压）。
5. **libx264 / openh264 的源码或库全盘都没有** —— FFmpeg 9 已删原生 h264 编码器，这是软件编码的硬缺口。
   - Win64 可退而使用 **h264_mf**（MediaFoundation，configure 默认 `[auto]`，Windows SDK 自带）。
   - Linux / Android 没有 MediaFoundation，**必须有 libx264（或 openh264）**。
6. **Linux 工具链完全没有**：无 WSL 发行版、无 Epic Linux 交叉组件（`v26_clang-20.1.8-rockylinux8`）、无 `aarch64-linux-gnu-gcc` → Linux amd64 与 arm64 均无法编译。
7. **Android 缺 arm64 的 libx264 静态库**（NDK 本身齐全）。
8. `diffutils`（cmp/diff）缺失，但 `configure` 不引用（0 处），可忽略。
9. **网络不可用**（HTTP/HTTPS 全部失败，无代理）→ 无法 pacman/winget 安装或下载源码，只能用上述离线缓存。
10. **DSH 沙箱内 MSYS2/Git Bash 起不来**（`couldn't create signal pipe, Win32 error 5`）→ 编译必须在「完全权限」下或普通终端里执行。

### 11.3 按平台结论

| 平台 | 可编性 | 说明 |
|---|---|---|
| Win64 | ✅ 基本可编（离线） | MSYS2 bash + NDK make + MSVC，`--toolchain=msvc --disable-x86asm`；H.264 用 `h264_mf`；libx264/NVENC 需额外源码 |
| Android arm64 | ⚠️ 工具齐、编码器缺 | NDK + make 就绪，但缺 libx264（MediaCodec 硬件编码在后台线程不实用） |
| Linux amd64 | ⛔ 不可编 | 缺整个 Linux 工具链 + libx264 |
| Linux arm64 | ⛔ 不可编 | 同上，另缺 aarch64 交叉编译器 |

### 11.4 恢复网络后需要补装

```bash
# MSYS2
pacman -S --needed make nasm diffutils pkgconf
# x264（三平台各自交叉编译）
git clone https://code.videolan.org/videolan/x264.git
# Linux：Epic Launcher 勾选 Linux 平台组件，或安装 WSL + gcc-aarch64-linux-gnu
# NVENC（可选）：nv-codec-headers
```

---

## 12. 系统 FFmpeg 开关 + 多版本适配（2026-10-26 18:25）

### 12.1 Build.cs 环境开关

| 变量 | 作用 |
|---|---|
| `UFFMPEG_SYSTEM_FFMPEG_ROOT` | 设置后改用该系统/vcpkg 前缀（需含 `include/`、`lib/`，Windows 还需 `bin/`），不再使用 `ThirdParty/ffmpeg` |
| 未设置 | 默认走仓库内 `ThirdParty/ffmpeg`（发布路径，9.x） |

- 构建时从所选头文件里**自动读取版本号**（`LIBAVCODEC_VERSION_MAJOR` 等），据此推导：
  - Windows 导入库名（`avcodec.lib`，注意 MSVC 无 `lib` 前缀；Linux 是 `libavcodec.a`）；
  - 运行时 DLL 名（`avcodec-<major>.dll`，自动适配 58/60/62/63）；
  - `UFFMPEG_SYSTEM_FFMPEG=1/0`、`UFFMPEG_SYSTEM_ROOT="..."` 两个编译宏。
- Android 在系统模式下会直接抛 `BuildException`（Android 没有系统 FFmpeg）。

用法：
```powershell
$env:UFFMPEG_SYSTEM_FFMPEG_ROOT='C:\WorkTool\vcpkg\installed\x64-windows'
& 'D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat' ffmpegEditor Win64 Development "-Project=F:\Project\UE5-ffmpeg\ffmpeg.uproject"
```
> ⚠️ UBT 的 makefile 缓存不感知环境变量变化：切换开关后要 `-Clean` 再编，或 touch 一下 `UFFmpeg.Build.cs`。

### 12.2 代码侧版本检测（`Public/FFmpegCompat.h`）

集中 `extern "C"` 头文件，并按库版本派生特性开关：

| 宏 | 条件 | 覆盖版本 |
|---|---|---|
| `UFFMPEG_HAVE_CHLAYOUT` | `LIBAVUTIL_VERSION_MAJOR >= 58` | 6.x / 8.x / 9.x |
| `UFFMPEG_HAVE_SWR_OPTS2` | 有 ch_layout 且 `LIBSWRESAMPLE_VERSION_MAJOR >= 4` | 6.x / 8.x / 9.x |
| `UFFMPEG_HAVE_OPT_SET_ARRAY` | `LIBAVUTIL_VERSION_MAJOR >= 60` | 8.x / 9.x |
| `UFFMPEG_HAVE_AV_PROFILE` | `LIBAVCODEC_VERSION_MAJOR >= 59` | 6.x / 8.x / 9.x |

对应代码分支（`FFmpegDirector.cpp`）：

| 位置 | 4.x | 6.x | 8.x | 9.x |
|---|---|---|---|---|
| 音频声道 | `ctx->channels/channel_layout` | `ch_layout` | `ch_layout` | `ch_layout` |
| 重采样 | `swr_alloc_set_opts` | `swr_alloc_set_opts2` | 同左 | 同左 |
| filter pix_fmts | `av_opt_set_int_list` | `av_opt_set_int_list` | `av_opt_set_array` | `av_opt_set_array` |
| H.264 profile | `FF_PROFILE_*` | `AV_PROFILE_*` | `AV_PROFILE_*` | `AV_PROFILE_*` |

运行时（`UFFmpeg.cpp`）：
- DLL 名由编译期版本宏生成（`avutil-61` 等），不再写死；
- 打印 `FFmpeg runtime '<version>' (avcodec x.y.z, avutil x.y.z); compiled against ...`；
- **运行时/头文件大版本不一致会打 Error**，便于发现 DLL 放错版本；
- 系统模式下自动 `PushDllDirectory(<root>/bin)` 并优先从该系统目录加载。
- 新增蓝图节点 `UFFmpegFunctionLibrary.GetFFmpegVersion()`。

### 12.3 验证矩阵

| 场景 | 命令 | 结果 |
|---|---|---|
| vendored 9.0.2（默认） | `-NoLink` | ✅ 编译通过（avcodec 63 / avutil 61） |
| 临时 4.0 前缀（git 里的旧头 + 旧库） | `-NoLink` + `UFFMPEG_SYSTEM_FFMPEG_ROOT` | ✅ 编译通过（avcodec 58 / avutil 56 / swresample 3） |
| 系统 vcpkg **8.0.1** | 完整链接（无 `-NoLink`） | ✅ 链接成功，产出 `UnrealEditor-UFFmpeg.dll` |
| 系统 8.0.1 运行 | 编辑器 `-nullrhi` 启动 | ✅ 日志：`FFmpeg runtime '8.0.1' (avcodec 62.11.100, avutil 60.8.100); compiled against avcodec 62 / avutil 60 / swresample 6`，无版本不匹配 |
| 系统 8.0.1 编码器 | 独立探针 | ✅ `h264_mf` 1080p30 打开成功；`rtmp`/`tcp` 协议存在 |
| 6.x | — | ⚠️ 本机无 6.x 头/库，未实测；分支组合已被 4.x 与 8.x 覆盖 |

### 12.4 注意

- 当前 `Plugins/UFFmpeg/Binaries/Win64/UnrealEditor-UFFmpeg.dll` 是**链接 vcpkg 8.0.1** 的产物（系统模式构建）。发布前要用 vendored 9.x 库重新链接。
- 系统模式仅用于开发机快速验证；发布必须自带库（尤其 Android/Linux）。
- 旧的 4.0 库仍保留在 `ThirdParty/ffmpeg/lib/vs`，仅用于本次兼容性验证，不参与默认构建。

---

## 13. PIE 里 h264_mf 报 EINVAL 的根因与修复（2026-10-26 18:40）

**现象**：PIE 调用录制 → 三次 `encoder 'h264_mf' cannot be opened: Invalid argument` → “No usable H.264 encoder” → `CreateFFmpegDirector` 返回 nullptr → 蓝图 “Accessed None ... On Record Finish”。

**根因：COM 套间不匹配（不是参数问题）**

FFmpeg 的 MediaFoundation 编码器在 `libavcodec/mf_utils.c: init_com_mf()` 里：

```c
hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
if (hr == RPC_E_CHANGED_MODE) { "COM must not be in STA mode"; return AVERROR(EINVAL); }
```

- **UE 游戏线程是 STA**（编辑器启动时初始化过 COM），所以 `avcodec_open2(h264_mf)` 必然返回 `-22 EINVAL`。
- 我之前的独立探针是控制台主线程（未初始化 COM），`CoInitializeEx(MTA)` 返回 S_OK，所以“参数完全相同却能成功”，误导了排查方向。

**实测证据**（同一进程内对比）：

| 调用线程 | 结果 |
|---|---|
| 主线程 `CoInitializeEx(STA)` 后 open | `-22 Invalid argument` |
| worker `CoInitializeEx(MTA)` 后 open | `0` 成功 |
| STA 主线程 + MTA worker open/encode/close | 30 帧、30 packet、写出 123KB mp4 ✅ |

**修复**（已实现并重新编译）：

1. `FFmpegDirector.cpp` 新增 `RunOnMtaComThread()`：在临时 MTA 线程上执行 `avcodec_open2`；
   - 打开失败时**在同一个 MTA 线程内** `avcodec_free_context`（因为 `mf_close → uninit_com_mf → CoUninitialize`，在 STA 线程调用会破坏 COM 计数）。
2. `FreeEncodingResources()` 释放视频编码器时也走 MTA 线程。
3. `EncoderThread.cpp` 的 `Init()` 里 `CoInitializeEx(MTA)`、`Exit()` 里 `CoUninitialize`，保证编码调用也在 MTA。
4. 失败日志补充了参数：`(%ux%u @ %d fps, %d bps)`，方便下定位。
5. 非 Windows 平台 `RunOnMtaComThread` 退化为直接调用。

> 注意：其他编码器（libx264 / NVENC / QSV / AMF）不依赖 COM 套间，此修复只影响 MF 路径。

**验证**：系统 FFmpeg 8.0.1 下重新编译链接成功（`Saved/Logs/Build_mta_fix.log`）。需要在 PIE 里复跑确认。

**蓝图侧建议**：`CreateFFmpegDirector` 失败会返回空，绑定 `On Record Finish` 前加 IsValid 判断，避免 “Accessed None”。

---

## 14. 参数语义与日志修复（2026-10-26 18:50）

| # | 问题 | 修复 |
|---|---|---|
| 1 | `FrameCount > TotalFrame` 多录 1 帧 | 改为 `>=`，恰好录 TotalFrame 帧 |
| 2 | 候选编码器按指针去重，`h264_mf` 被尝试 2 次（加兜底共 3 次 Warning） | 改成 `FCStringAnsi::Strcmp` 字符串比较去重 |
| 3 | `SoundVolume` 超范围静默 clamp | 超出 `[0,8]` 打 Warning 并提示“1.0=原音量、0.5=50%”；>2.0 额外提示会削波 |
| 4 | `VideoLength` 语义是帧数，和“时长”直觉不符 | 新增蓝图参数 **`bVideoLengthInSeconds`（默认 true）**：true 时按秒换算 `TotalFrame = round(秒 × FPS)`；false 保持帧数语义 |
| 5 | 启动日志缺关键参数 | 现在打印：编码器、采集→输出分辨率、fps、bps、总帧数与时长、length 单位、音量、输出路径 |

- 签名变更：`Initialize_Director(...)` / `CreateFFmpegDirector(...)` 末尾新增 `bool bVideoLengthInSeconds = true`。
  已有蓝图节点会自动多出该引脚（默认勾选=按秒），**建议在编辑器里右键节点 Refresh Node 确认**。
- 例：`VideoLength=2` + `FPS=30` + 默认按秒 → 60 帧 / 2.0 s；若要帧数语义则取消勾选。
- `VideoLength <= 0` 会打 Warning 并立即停止录制。

验证：`Saved/Logs/Build_params_fix.log` 编译链接成功（系统 FFmpeg 8.0.1）。

---

## 15. 输出路径中文/非法字符导致 avio_open EINVAL（2026-10-26 18:55）

**现象**：`avio_open('F:/Project/UE5-ffmpeg/2026?10?26? ??6:44:53Example.mp4') failed: Invalid argument`

**根因**：蓝图文件名由 `FDateTime` 拼成（含“年/月/日/下午”等中文），代码用 `TCHAR_TO_ANSI` 转给 FFmpeg，中文被替换成 `?`；而 `?` 是 Windows 文件名非法字符 → `avio_open` 返回 `-22 EINVAL`。
另外时间戳里的 **`:`** 在 Windows 文件名中同样非法。

**实测**（FFmpeg 8.0.1，同一进程）：

| 路径 | 结果 |
|---|---|
| `plain_2026.mp4` | `0`，文件创建 |
| `测试_2026.mp4`（UTF-8） | `0`，文件创建成功 ✅ |
| `2026?10?26?...mp4`（ANSI 乱码） | `-22 Invalid argument` |

**修复**（`FFmpegDirector.cpp`）：

1. 输出路径一律改用 **`TCHAR_TO_UTF8`**：`avformat_alloc_output_context2`、`Create_Video_Encoder`（内部 `avio_open`）。
2. 新增 `SanitizeWindowsPath()`：把 `< > : " | ? *` 替换为 `_`（保留盘符 `:`，URL 不处理），并打 Warning 显示“原路径 → 实际路径”。
3. `FileAddr` 与 `OnRecordFinish` 使用**清洗后的路径**。
4. `avio_open` 失败提示按场景区分（RTMP → 检查网络；本地文件 → 检查目录/权限）。
5. 错误信息用 `UTF8_TO_TCHAR` 打印，中文路径不再乱码。

示例：`2026年10月26日 下午6:44:53Example.mp4` → `2026年10月26日 下午6_44_53Example.mp4`（中文保留，冒号变下划线）。

**建议**：蓝图里时间戳尽量用 ASCII 格式（`yyyy-MM-dd_HH-mm-ss`），跨 Windows/Linux/Android 都安全。

验证：`Saved/Logs/Build_path_fix.log`、`Build_path_sanitize.log` 编译链接成功。

---

## 16. 链接策略调整：非 Android 用系统库（2026-10-26 19:05）

**新规则**（已写入 `ThirdParty/ffmpeg/BUILD_9.md`）：

| 平台 | 来源 |
|---|---|
| **Android** | 仓库自带 `ThirdParty/ffmpeg`（include + `lib/Android/<abi>/libav*.a`） |
| **Win64 / Linux / LinuxArm64 / Mac** | **系统安装的 FFmpeg**（开发包） |

`UFFmpeg.Build.cs` 不再依赖“是否设置环境变量”来切换，而是按平台固定策略；`UFFMPEG_SYSTEM_FFMPEG_ROOT` 变成**可选覆盖**（自定义前缀/交叉 sysroot）。

系统前缀解析顺序：
1. `UFFMPEG_SYSTEM_FFMPEG_ROOT`（需含 `include/libavcodec/avcodec.h`）
2. Windows：`%VCPKG_ROOT%/installed/x64-windows` → `VCPKG_INSTALLATION_ROOT` → `C:\vcpkg` → `C:\WorkTool\vcpkg`
   Linux：`/usr/local` → `/usr`；macOS：`/opt/homebrew` → `/usr/local`
3. 找不到直接抛 `BuildException`，并打印各平台安装命令。

其它行为：
- 版本号仍从所选头文件读取，DLL/导入库名自动生成；编译宏 `UFFMPEG_SYSTEM_FFMPEG=1/0` 与 `UFFMPEG_SYSTEM_ROOT` 在系统模式下写入（运行时据此加载 DLL）。
- 系统模式的 DLL **不会**被 UE 自动 stage（打包需自行分发或目标机安装）。
- Android 分支忽略 `UFFMPEG_SYSTEM_FFMPEG_ROOT`，始终用仓库内库。

**验证**：
- Win64 全清后重建成功，自动发现 `C:\WorkTool\vcpkg\installed\x64-windows`（avcodec 62 / avutil 60），链接成功（`Saved/Logs/Build_policy_win64_clean.log`）。
- Android arm64 编译通过，使用仓库内 9.0.2 头文件（`Saved/Logs/Build_policy_android.log`）。
- Linux / LinuxArm64 仍受限于本机无 Linux 工具链，无法验证；在 Linux 主机上装 `libav*-dev` 即可（或把 ffmpeg 装进交叉 sysroot 后用 `UFFMPEG_SYSTEM_FFMPEG_ROOT` 指过去）。

---

## 17. 滤镜图 pix_fmts 设置失败（2026-10-26 19:10）

**现象**：`Failed to constrain the filter output pixel format: Invalid argument`

**根因**：FFmpeg 6+（这里 8.0.1）把 buffersink 的 `pix_fmts` 标记为非 runtime 选项。过滤器经
`avfilter_graph_create_filter` 创建后已经 init，再调用 `av_opt_set_array` / `av_opt_set_int_list`
都会失败，底层日志是：

```
Option 'pix_fmts' is not a runtime option and so cannot be set after the object has been initialized
```

（返回 -22 EINVAL，与参数类型无关；`set_array(PIXEL_FMT)`、`set_array(INT)`、`set_int_list` 三种写法全部 -22，探针已确认。）

**修复**：

- 不再设置 buffersink 的 `pix_fmts`；
- 改为在滤镜图里用 `format` 滤镜强制输出格式：
  `[in]scale=W:H,format=yuv420p[out]`
- 删除 `av_opt_set_array` / `av_opt_set_int_list` 分支与 `UFFMPEG_HAVE_OPT_SET_ARRAY` 宏（已无用）。

**验证**：

| 测试 | 结果 |
|---|---|
| 滤镜图 `[in]scale=1920:1080,format=yuv420p[out]` parse/config | `0 / 0` |
| 端到端（buffer→scale,format→buffersink→h264_mf→mp4，MTA 线程） | `packets=30`，输出 275 KB mp4 ✅ |

编译：`Saved/Logs/Build_filter_fix.log` 成功。

---

## 18. UObjectHash 崩溃：CircleQueue 重复销毁（2026-10-27 09:45）

**现象**：

```
Fatal error: [UObjectHash.cpp] [Line: 77]
Unidentified failure for object CircleQueue /Engine/Transient.CircleQueue_1, hash itself may be corrupted or buggy.
```

**根因**：`UCircleQueue` 原本是 `UCLASS` + `NewObject` + `AddToRoot`，而 `FEncoderThread::~FEncoderThread()` 里这样销毁：

```cpp
queue->RemoveFromRoot();
queue->ConditionalBeginDestroy();
queue->BeginDestroy();     // ← 第二次销毁
```

`ConditionalBeginDestroy()` 已经会执行 `BeginDestroy()`，再手动调一次就是从 UObject 哈希里移除两次，UE 5.8 直接 fatal。

**修复**（彻底去掉 UObject 依赖）：

1. `UCircleQueue` 改为**普通 C++ 类**：删除 `UCLASS()` / `GENERATED_BODY()` / `: public UObject`，删除 `#include "EncodeData.generated.h"`。
2. `FEncoderThread` 用 `new` / `delete` 管理三个队列（`CreateQueue` 与析构函数）。
3. 队列指针初始化为 `nullptr`；`IsQueneEmpty()` / `RunEncode()` / `EncodeAudio()` / `InsertAudio()` 增加空指针保护。
4. 不再 `AddToRoot` / `RemoveFromRoot`——队列只被编码线程对象独占，不需要 GC。

**为什么之前没炸**：4.x 时代 UObject 销毁路径较宽松；5.8 对哈希一致性检查更严。

**验证**：`Saved/Logs/Build_queue_fix2.log` 编译链接成功。运行时需在 PIE 里复测（尤其“录完/停止/退出 PIE”三个阶段）。

> 备注：链接失败过一次，原因是上一次 fatal 留下的 `CrashReportClientEditor.exe` 占着插件 DLL，结束该进程后正常。

---

## 19. UFFmpegDirector 的 GC root 泄漏（2026-10-27 09:55）

**问题**：`CreateFFmpegDirector` 成功后调用 `Director->AddToRoot()`，但录制结束/销毁时从不 `RemoveFromRoot()` → 每录一次泄漏一个 `UFFmpegDirector`（对象本身；FFmpeg 资源在 Stop 时已释放）。

**修复**（`FFmpegDirector.cpp`）：

| 位置 | 行为 |
|---|---|
| `Stop()` 末尾（广播 `OnRecordFinish` 之后） | `RemoveFromRoot()` |
| `DestoryDirector()` 末尾（`Encode_Finish()` 之后） | `RemoveFromRoot()` |
| `AddTickTime()` | 新增兜底：若 `bInitialized && FD_world->bIsTearingDown`（PIE 结束但没有走 Stop），则杀编码线程、`Encode_Finish()`、`SubmixListener.Reset()`、`RemoveFromRoot()`，并返回 false 停掉 ticker；**不触碰 AudioDevice/Slate**（此时它们已在销毁中） |

- 重复 `RemoveFromRoot()` 是安全的（非 rooted 时无操作），所以 Stop + DestoryDirector 都调用没问题。
- 注意：取消 root 后，如果蓝图没有把返回值存到变量里，对象会在下次 GC 被回收；若之后还要读 `GetLastError()`/`GetVideoEncoderName()`，请在蓝图里保留引用。

**验证**：`Saved/Logs/Build_root_release.log`、`Build_leak_fix.log` 编译链接成功；需要在 PIE 里连续录制两次并退出确认无泄漏/无崩溃。

---

## 20. VideoFilter 判空：为空时用当前视口分辨率（2026-10-27 10:15）

**改动**（`FFmpegDirector.cpp` 的 `Initialize_Director`）：

- 先取视口尺寸，截断为偶数得到默认输出分辨率：
  - `out_width = ViewportWidth & ~1`
  - `out_height = ViewportHeight & ~1`
  - 采集缓冲仍用 32 对齐的 `width = FormatSize_X(ViewportWidth)` 与偶数 `height`。
- `VideoFilter.TrimStartAndEnd()` 为空时：打 Log 说明“使用当前视口分辨率”，保持上面的默认值。
- 非空时按 `scale=W:H` 解析覆盖输出分辨率；格式不对或数值越界时打 Warning，并回退到视口分辨率（两种失败分支都有明确日志）。
- 视口尺寸校验同时覆盖采集与输出尺寸（2..8192）。

编译：`Saved/Logs/Build_viewport_filter.log` 成功。

> 备注：视口宽度不是 32 的倍数时，采集宽度会向上对齐（例如 1367 → 1376），输出按视口偶数尺寸（1366）缩放，可能包含极窄的边缘像素；如果需要像素级精确，建议把视口设成 32 对齐的尺寸。

---

## 21. 录制画面逐行错位（stride/pitch 不匹配，2026-10-27 10:20）

**现象**：录出来的视频整体是斜向撕裂/拖影，每行相对上一行横向偏移；实际渲染画面正常。

**根因**：采集宽度被 `FormatSize_X()` 向上对齐到 32 的倍数，但源缓冲的行是按**纹理真实宽度**紧凑排列的：

- D3D12/Vulkan 分支：`ReadSurfaceData` 得到的是 `TexWidth * TexHeight` 紧凑数组，代码却按 `LolStride = width*4`（32 对齐后）去读 `width` 像素/行；
- D3D11 分支：`width` 大于纹理宽度时，每行会多读若干像素，跨到下一行。

只要 `width != TexWidth`（例如视口 1289 → width 1312），每行就累积 `width - TexWidth` 像素的偏移，形成截图中的斜向撕裂。

**修复**：

1. **去掉 32 对齐**：`width = ViewportWidth & ~1`、`height = ViewportHeight & ~1`（YUV420P 只需偶数），删除 `FormatSize_X()`。
2. **D3D12/Vulkan 改为自洽布局**：分配 `width*height` 缓冲、清零、`LolStride = width*4`，按 `min(width, TexWidth) × min(height, TexHeight)` 拷贝并做原有 BGRA 打包；多出的边缘保持黑色。
3. **D3D11 使用锁返回的真实 pitch**（`LolStride`），`width ≤ TexWidth` 时每行读取是安全的。
4. 新增保护：若 back buffer 尺寸小于采集尺寸（窗口被缩小），跳过该帧并只提示一次。
5. 保留 `VideoFilter` 的缩放：采集尺寸是视口偶数尺寸，输出尺寸可被 `scale=W:H` 覆盖。

**编译**：`Saved/Logs/Build_stride_fix2.log` 成功。

> 构建小坑：编辑器开着时 UBT 会因 `Saved/Config/WindowsEditor/EditorPerProjectUserSettings.ini` 被占用而崩在 HotReload 配置读取；加 `-NoHotReloadFromIDE` 可绕过。

---

## 22. 录制画面整体偏红（像素通道位重叠，2026-10-27 10:35）

**现象**：偏移修好后画面正常，但整体偏粉/偏红（天空变紫、地面变粉）。

**根因**：D3D12/Vulkan 分支的像素打包把 **Alpha 和 Red 写进了重叠的 bit**：

```cpp
DestRow[Col] = (c.A << 0) | (c.B << 22) | (c.G << 12) | (c.R << 2);
```

- R 占 bit 2..9，A<<0 占 bit 0..7 → bit 2..7 重叠；
- 背缓冲 A 通常是 255，于是 `R' = (P>>2)&0xFF = R | 0x3F`，红通道被抬高最多 +63 且低位置 1；
- G/B 的提取区间不重叠，所以只有红色被抬高 → 粉红/发红。

**修复**：统一用标准 **BGRA** 布局，不再做自定义位打包。

1. D3D12/Vulkan：`ReadSurfaceData` 的 `FColor` 直接 `FMemory::Memcpy` 到 `width*height` 缓冲（`FColor` 内存序就是 B,G,R,A，与 D3D 背缓冲一致）。
2. `Encode_Video_Frame` 提取改为：
   ```cpp
   R = (Pixel >> 16) & 0xFF;
   G = (Pixel >> 8) & 0xFF;
   B = Pixel & 0xFF;
   ```
   （写入 `buff_bgr` 的顺序是 B,G,R，匹配 `AV_PIX_FMT_BGR24`。）
3. D3D11 直接锁纹理的分支本来也是 BGRA，因此现在两条路径的像素解释一致。

**验证**：`Saved/Logs/Build_color_fix.log` 编译链接成功；需要重新录制确认颜色恢复。

---

## 23. 蓝图注释：VideoFilter 格式（2026-10-27 10:50）

**改动**（两个 BlueprintCallable 函数）：

| 位置 | 内容 |
|---|---|
| `UFFmpegFunctionLibrary::CreateFFmpegDirector` | 新增完整 doc 注释（输出为 UHT 的 `Comment` 元数据）+ `meta=(ToolTip=...)`，开头即写明 **VideoFilter format: "scale=WIDTH:HEIGHT"，如 scale=1920:1080，留空则用当前视口分辨率** |
| 同上 `VideoFilter` 参数 | `UPARAM(meta=(ToolTip="Output scaling, format: scale=WIDTH:HEIGHT ..."))` —— 悬停在该引脚上显示 |
| `UFFmpegDirector::Initialize_Director` | 同样加 `meta=(ToolTip=...)` 与注释，保持两处说明一致 |

注释里同时说明了：留空/非法格式回退视口分辨率、宽高向下取偶、返回 nullptr 时用 `GetLastError()` 查原因。

**验证**：UHT 生成代码中已确认存在：

```
{ "ToolTip", "Starts recording ... VideoFilter format: \"scale=WIDTH:HEIGHT\", e.g. scale=1920:1080 ..." },
{ "ToolTip", "Output scaling, format: scale=WIDTH:HEIGHT ..." },
```

编译：`Saved/Logs/Build_bp_tooltip.log` 成功。

> 注意：正在运行的编辑器加载的是旧插件 DLL，**重启编辑器**后新的节点提示才会生效。

---

## 24. 公开 IsRtmp 开关，适配 RTMP 推流（2026-10-27 11:05）

**改动**：`CreateFFmpegDirector` 与 `Initialize_Director` 末尾新增蓝图参数 **`bIsRtmp`（默认 false）**。

| 参数 | 行为 |
|---|---|
| `bIsRtmp = true` | 强制把 `OutFileName` 当 RTMP URL：使用 **flv** muxer、**不做 Windows 文件名清洗**；若字符串里没有 `://` 会打 Warning（提示可能不是合法 URL） |
| `bIsRtmp = false`（默认） | 当本地文件处理：按扩展名选 muxer、执行非法字符替换；**但以 `rtmp` 开头的字符串仍会自动识别为推流**，保证已有蓝图不用改 |

其它连带修改：

- 新增成员 `bool bRtmpOutput`，`avio_open` 失败提示按 RTMP/本地文件分别给出；
- 启动日志追加输出类型：
  `output=C:/x.mp4 (file)` 或 `output=rtmp://host/app/key (RTMP/FLV)`；
- `bIsRtmp` 引脚带 `UPARAM(meta=(ToolTip=...))`，函数 ToolTip 也补充了推流说明；
- UHT 生成结果已确认：`bIsRtmp` 默认 `false` + ToolTip。

**示例**：
```
OutFileName = rtmp://live.example.com/app/streamkey
IsRtmp      = true      // 也可以留 false，rtmp:// 会被自动识别
VideoFilter = 空         // 视口分辨率；推流通常建议 scale=1920:1080 + 30fps + 6000000 bps
```

编译：`Saved/Logs/Build_rtmp_flag.log` 成功。重启编辑器后蓝图节点末尾会出现 **Is Rtmp** 引脚。

---

## 25. Android 静态库编译环境验证（2026-10-27 11:20）

**结论：✅ 本机可以编译 Android arm64 的 FFmpeg 静态库**，已实测 configure + make 全流程通过，产出 6 个 AArch64 `.a`。

### 已验证的环境事实

| 项 | 状态 |
|---|---|
| NDK | r27c（clang 18，`aarch64-linux-android29`），`.cmd` 包装器可被 MSYS2 bash 直接执行 |
| NDK make | 4.3 可用（MSYS2 自身**没有 make**，必须用 NDK 的） |
| MSYS2 | `C:\WorkTool\msys2`：bash/sed/awk/grep/tr/sort/install/perl 齐全 |
| 宿主 C 编译器 | MSYS2 **没有 gcc** → 用 **VS 自带 clang 22**（vcvars64 环境）作 `--host-cc=clang` 通过 C11 检查 |
| cmp/diff | 缺失（diffutils），只影响 `cp_if_changed` 的“未变化”提示；可用 Git 的 `usr/bin/cmp.exe` 补，忽略也可 |
| nasm / pkg-config | 不需要（arm64 不用 x86 asm；`--disable-autodetect` 跳过 pkg-config） |

### 关键坑：路径风格

configure 内部用 `pwd -P`，生成的 `SRC_PATH` 是 MSYS 风格 `/f/Project/...`，而**原生 Windows make 不认**。构建前需：

```bash
sed -i 's|/f/Project/UE5-ffmpeg/ffmpeg-9.0.2|F:/Project/UE5-ffmpeg/ffmpeg-9.0.2|g' \
  _android_build/Makefile _android_build/ffbuild/config.mak
```

再 `make -j8 SHELL=C:/WorkTool/msys2/usr/bin/sh.exe`。

### 实测配置与产物

```bash
--target-os=android --arch=aarch64 --cpu=armv8-a --enable-cross-compile --host-cc=clang \
--cc=$NDK/.../aarch64-linux-android29-clang.cmd --cxx=...clang++.cmd \
--ar=llvm-ar --ranlib=llvm-ranlib --strip=llvm-strip --nm=llvm-nm \
--enable-static --disable-shared --disable-programs --disable-doc --disable-avdevice --disable-autodetect
```

产物（`_android_build/libav*/`，可直接拷到 `ThirdParty/ffmpeg/lib/Android/arm64-v8a/`）：

| 库 | 大小 |
|---|---|
| libavcodec.a | 131 MB |
| libavformat.a | 47 MB |
| libavfilter.a | 37 MB |
| libswscale.a | 8.5 MB |
| libavutil.a | 4.4 MB |
| libswresample.a | 0.6 MB |

架构校验：`ELF64 / REL / AArch64`；符号含 `ff_rtmp_protocol`、`ff_flv_muxer`、`avformat_network_init`、`ff_aac_encoder`。

### 仍然缺：H.264 编码器

当前配置：`CONFIG_LIBX264_ENCODER=0`、`CONFIG_H264_MEDIACODEC_ENCODER=0`、`CONFIG_H264_ENCODER=0` → **没有任何 H.264 编码器**（FFmpeg 9 已移除原生 h264 编码器）。

要在 Android 上真正录制/推流，三选一：

1. 交叉编译 **libx264** 再重配 FFmpeg（当前无源码、无网络）；
2. `--enable-jni --enable-mediacodec` 启用 `h264_mediacodec`（硬件编码，需要 Surface 输入，插件采集层要改造）；
3. 换用带 libx264 的预编译 Android FFmpeg。

RTMP / FLV / AAC 均已就绪。

---

## 26. Android FFmpeg 安装 libx264 并打通 APK（2026-10-27 11:45）

**结论：已完成**。x264 交叉编译 → FFmpeg 带 `--enable-libx264` 重编 → 装入插件 → Android 完整构建/打包 APK 成功，产物含 x264 符号。

### 前置：网络在沙箱内被拦

- 沙箱（workspace-write）下所有 HTTPS 都失败；**完全权限（danger-full-access）下 GitHub / VideoLAN 均 200**。
- 因此克隆 x264 必须用完全权限执行。

### 补齐的工具

| 缺什么 | 解决 |
|---|---|
| make | 用 NDK 自带 `prebuilt/windows-x86_64/bin/make.exe`（配合 `SHELL=<msys sh>`） |
| 宿主 gcc | 用 VS 自带 **clang 22**（`VC\Tools\Llvm\x64\bin`，需 vcvars64 环境）作为 `--host-cc=clang` |
| `strings` | x264 endian 检测需要 → `STRINGS=<ndk>/llvm-strings.exe` |
| pkg-config | 从缓存包 `C:\WorkTool\vcpkg\downloads\msys2-pkgconf-2.5.1-1-x86_64.pkg.tar.zst` 解压安装到 `C:\WorkTool\msys2` |

### x264 编译

```bash
git clone --depth 1 https://code.videolan.org/videolan/x264.git   # 完全权限下
cd x264
CC=$TC/bin/aarch64-linux-android29-clang \
AR=$TC/bin/llvm-ar.exe RANLIB=$TC/bin/llvm-ranlib.exe \
STRIP=$TC/bin/llvm-strip.exe NM=$TC/bin/llvm-nm.exe STRINGS=$TC/bin/llvm-strings.exe \
./configure --host=aarch64-linux-android --sysroot=$TC/sysroot \
  --enable-static --disable-cli --enable-pic \
  --prefix=/f/Project/UE5-ffmpeg/_x264_android \
  --disable-opencl --disable-avs --disable-swscale --disable-lavf --disable-ffms --disable-gpac --disable-lsmash
"$NDK/prebuilt/windows-x86_64/bin/make.exe" -j8 SHELL=C:/WorkTool/msys2/usr/bin/sh.exe
make install
```

产物：`libx264.a`（AArch64，`x264_encoder_open_165`）、`x264.h`、`x264.pc`。

### FFmpeg 重配（关键点）

1. 把 `_x264_android/lib/pkgconfig/x264.pc` 的 `prefix=` 改成 **`F:/...` Windows 风格**，`PKG_CONFIG_PATH` 指向该目录（否则原生 clang 不认 `/f/...`）。
2. 重新 configure，追加 `--enable-gpl --enable-libx264`，其余参数同第 25 节。
3. 构建前仍要 `sed` 修正 `SRC_PATH` 为 `F:/...`。
4. **HOSTCC 必须在 PATH 里**（VS LLVM 目录 + vcvars），否则 `libswscale/aarch64/ops_asmgen.exe` 编不过。

结果：`CONFIG_LIBX264_ENCODER=1`，`libavcodec.a` 中有 18 处 x264 未定义引用；6 个库约 232 MB。

### 安装到插件

`Plugins/UFFmpeg/ThirdParty/ffmpeg/lib/Android/arm64-v8a/` 放入 6 个 FFmpeg 库 + `libx264.a`（合计 233.4 MB）；`UFFmpeg.Build.cs` 的 Android 分支在 ffmpeg 库之后追加 `libx264.a`。

### 验证

- Android 完整构建成功（含 Gradle 打包）：`Binaries/Android/ffmpeg-arm64.apk`（128 MB）。
- `Binaries/Android/ffmpeg_Symbols_v1/ffmpegarm64/libUnreal.so` 中确认存在 `x264_encoder_open_165`、`x264_encoder_encode`。

### 许可与清理

- `--enable-gpl + --enable-libx264` → Android 自带 FFmpeg 为 **GPLv2+**，分发 APK 需遵守 GPL。
- 保留的中间目录：`x264/`（源码 9.9 MB）、`_x264_android/`（前缀 2.1 MB）、`_android_build/`（构建树 468 MB，可删，库已装进插件）。

---

## 27. 静态库体积：调试符号占 87%（2026-10-27 11:52）

**问题**：Android 静态库合计 233.4 MB，偏大。

**检查结论**：FFmpeg configure **默认开启 debug**（`--disable-debug` 可关），生成的 `ffbuild/config.mak` 里 `CFLAGS` 含 **`-g`**，所以 DWARF 全进了 `.o`/`.a`。实测 `libavcodec.a`：

| | 大小 |
|---|---|
| 带 `-g` | 131.6 MB |
| `llvm-strip -g` 后 | 17.4 MB |
| 占比 | **调试符号约 86.8%** |

成员对象的段表里可见 `.debug_info/.debug_loc/.debug_abbrev/.debug_ranges/.debug_str`。

x264 不受影响（configure 输出 `debug: no`，无 `-g`）。

**修复**：重新 configure，追加 `--disable-debug`（其余参数不变），清洁重编。

新体积（安装到插件的 `ThirdParty/ffmpeg/lib/Android/arm64-v8a/`）：

| 库 | 大小 |
|---|---|
| libavcodec.a | 17.4 MB |
| libavfilter.a | 6.2 MB |
| libavformat.a | 5.2 MB |
| libswscale.a | 1.7 MB |
| libavutil.a | 1.1 MB |
| libswresample.a | 0.1 MB |
| libx264.a | 2.0 MB |
| **合计** | **33.8 MB**（原 233.4 MB，-85.5%） |

验证：7 个库的成员对象 `.debug` 段数量均为 **0**；`CFLAGS` 中不再有 `-g`；Android 完整构建 + APK 打包成功。

> 注意：**APK 体积不变（128.2 MB）**——Gradle 的 `stripDebugDebugSymbols` 本来就会把原生库的符号剥掉，调试信息只存在于静态库和 `ffmpeg_Symbols_v1/…/libUnreal.so`（该文件从 467.4 MB 降到 442.9 MB）。所以这次优化主要省的是仓库体积、链接时间和构建磁盘占用，不影响 APK。



















