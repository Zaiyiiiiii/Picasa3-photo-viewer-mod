# Picasa 照片查看器增强补丁 / Picasa Photo Viewer Mod

[中文](#中文) | [English](#english)

---

## 中文

给 Picasa 3.9 自带的照片查看器（`PicasaPhotoViewer.exe`）加上现代格式、动图、视频播放和高 DPI 显示。原版界面保持不变。

补丁只有一个文件 `version.dll`，放在查看器 exe 的旁边，程序启动时会自动加载它。安装目录里的原版文件一个都不改，删掉 `version.dll` 就完全恢复原样。

### 功能

**新格式**
- 静态图片：WebP、HEIC/HEIF、AVIF、JPEG XL，以及系统 WIC 支持的其他格式。
- 动图：GIF、动态 WebP、动态 AVIF、动态 JPEG XL，直接在查看器里循环播放。
- 视频：直接在查看器内播放，带声音。原版只能调用外部播放器。
  - 支持的扩展名：mp4、m4v、mov、mkv、webm、avi、wmv、flv、ts/m2ts/mts、mpg/mpeg、vob、ogv、3gp、mxf 等。
- 打开对话框（Ctrl+O）列出全部支持的格式。

**显示**
- 色彩管理：读取图片内嵌的 ICC 配置文件和 CICP/nclx 色彩信息（P3、BT.2020 等），并转换到显示器的色彩空间。设置里的“色彩管理”开关控制是否启用。
- 高 DPI：查看器按显示器的缩放比例渲染，界面、按钮、文字和底部胶片条按比例放大，不再模糊。

**去掉的内容**
- 所有依赖 Picasa 主程序、图库或网络服务的功能：
  - 在 Picasa 中编辑/定位、打印、电子邮件、博客、上传、星标；
  - 对应的工具栏按钮和右键菜单项。
- 查看器所有的网络访问都被拦截。
- 不再读写 `.picasa.ini`。旋转只影响当前查看，不写入文件。

**设置**
- 原来的“更多”按钮换成了设置按钮，点开是一个精简的设置窗口：
  - 文件关联：按格式勾选，然后点按钮打开 Windows“默认应用”页面完成设置（Windows 10/11 不允许程序自行抢占默认应用）。
  - 界面选项：滚轮缩放、自动隐藏操作栏、全屏启动、透明背景、色彩管理。
- 原来的设置窗口（带谷歌登录选项卡）不再使用。

### 安装

1. 需要已安装 Picasa 3.9（版本 3.9.0），或者任何一份完整的原版安装目录。
2. 把 `version.dll` 复制到 `PicasaPhotoViewer.exe` 所在目录。默认路径是 `C:\Program Files (x86)\Google\Picasa3\`。
3. 正常打开图片即可。
4. 要让双击图片或视频时用这个查看器打开，就点底栏的设置按钮，勾选格式，再点“打开 Windows 默认应用设置”，在系统页面里把对应格式指定给“Picasa Photo Viewer”。

卸载：删除 `version.dll`。如果做过文件关联，先在设置里取消勾选，再删除这个文件。

### 运行时生成的文件

- `%LOCALAPPDATA%\PicasaPhotoViewerMod\respack-*.yt`：界面资源包的修改版副本，作用是隐藏按钮、替换图标。补丁在首次运行时根据原版资源包生成它。可以随时删除，下次启动时会重新生成。
- `picasa_mod.log`：补丁日志，写在 `version.dll` 旁边。如果那个目录没有写权限，就不会写日志。

### 环境变量（排查问题用）

| 变量 | 作用 |
|---|---|
| `PICASA_MOD_DPI=0` | 不启用高 DPI，恢复原版的系统缩放（画面会变模糊）。 |
| `PICASA_MOD_NOHW` | 视频不用硬件解码。 |
| `PICASA_MOD_MUTE` | 视频静音。 |

### 限制

- 查看器是 32 位程序，可用内存最多约 2GB。超大图片或高分辨率长视频可能因此受限。
- 视频没有进度条，只能播放和循环。

### 从源码构建

需要：
- Visual Studio 2022（C++ 工作负载）、CMake、Ninja；
- 仓库同级目录下的 `vcpkg`；
- 同级目录下的原版安装 `picasa3-64.3.9.0`，用作 `dist` 的来源，只会读取。

```bash
tools\deps.cmd
```

```bash
tools\build.cmd
```

`tools\build.cmd` 生成 `build\mod\version.dll`，并把原版安装目录加上补丁组装到 `dist\`，供本地运行测试。

```bash
tools\release.cmd
```

`tools\release.cmd` 先构建，再把 `version.dll` 和本说明打包成 `release\picasa-viewer-mod-<版本>.zip`。发布包里不含任何 Google 的文件。

GitHub Actions 会在每次推送时自动构建，构建结果在运行页面的 Artifacts 里下载。推送 `v*` 标签时，会自动发布对应版本的 Release。

### 源码结构

| 路径 | 内容 |
|---|---|
| `mod/src/version_proxy.cpp` | 代理系统 `version.dll` 的导出 |
| `mod/src/dllmain.cpp` | 依次安装各个挂钩 |
| `mod/src/filetypes.cpp` | 扩展名到类型的映射、打开对话框的过滤器、文件关联表 |
| `mod/src/decoder.cpp`、`mod/src/codecs/` | 解码器（WebP、HEIF、AVIF、JXL、GIF、WIC、FFmpeg 视频、WASAPI 音频）和色彩管理 |
| `mod/src/animator.cpp` | 动图和视频的逐帧显示 |
| `mod/src/dpi.cpp`、`mod/src/uiscale.cpp` | DPI 感知和界面缩放 |
| `mod/src/respack.cpp` | 在运行时生成资源包的修改版副本 |
| `mod/src/settings.cpp` | 新的设置窗口和文件关联 |
| `mod/src/picasa_features.cpp` | 去掉 Picasa 相关功能，把“更多”按钮改成设置按钮 |
| `mod/src/network.cpp` | 拦截网络访问 |

Picasa 是 Google 的软件。本项目不包含也不分发 Picasa 的任何文件。

---

## English

Adds modern image formats, animation, in-viewer video playback and high-DPI rendering to the photo viewer that ships with Picasa 3.9 (`PicasaPhotoViewer.exe`). The original interface is kept as it is.

The mod is a single file, `version.dll`. Put it next to the viewer's exe and the viewer loads it on start. None of the installed files are changed: deleting `version.dll` restores the original viewer completely.

### Features

**New formats**
- Still images: WebP, HEIC/HEIF, AVIF, JPEG XL, and anything else the system's WIC codecs can read.
- Animation: GIF, animated WebP, animated AVIF and animated JPEG XL play in a loop inside the viewer.
- Video: plays inside the viewer, with sound. The original only handed videos to an external player.
  - Extensions: mp4, m4v, mov, mkv, webm, avi, wmv, flv, ts/m2ts/mts, mpg/mpeg, vob, ogv, 3gp, mxf and more.
- The Open dialog (Ctrl+O) lists every supported format.

**Display**
- Color management: embedded ICC profiles and CICP/nclx color information (P3, BT.2020, ...) are converted to the display's color space. The "color management" option in the settings turns this on or off.
- High DPI: the viewer renders at the display's scale; the interface, buttons, text and filmstrip are scaled to match instead of being blurred.

**Removed**
- Everything that depends on the Picasa application, its library or its web services:
  - Edit/Locate in Picasa, Print, Email, Blog This, Upload, Star;
  - the matching toolbar buttons and context menu items.
- All network access from the viewer is blocked.
- `.picasa.ini` is no longer read or written. Rotation only affects the current view and is not saved.

**Settings**
- The "More" button is now a settings button that opens a small settings window:
  - File associations: tick formats, then use the button to open the Windows "Default apps" page and finish there (Windows 10/11 do not let programs take over default apps by themselves).
  - Interface options: mouse-wheel zoom, auto-hide action bar, start in full screen, transparent background, color management.
- The original settings window (with the Google sign-in tab) is no longer used.

### Installation

1. You need Picasa 3.9 (version 3.9.0) installed, or any complete copy of its install folder.
2. Copy `version.dll` into the folder containing `PicasaPhotoViewer.exe`. The default is `C:\Program Files (x86)\Google\Picasa3\`.
3. Open images as usual.
4. To open images or videos with this viewer on double-click: click the settings button in the bottom bar, tick the formats, click "打开 Windows 默认应用设置" (Open Windows default apps), and assign the formats to "Picasa Photo Viewer" on that page.

Uninstall: delete `version.dll`. If you set up file associations, untick them in the settings first.

### Files created at run time

- `%LOCALAPPDATA%\PicasaPhotoViewerMod\respack-*.yt`: a modified copy of the interface resource pack (hidden buttons, replaced icon), generated from the original pack on first run. Safe to delete; it is regenerated on the next start.
- `picasa_mod.log`: the mod's log, written next to `version.dll`. Nothing is logged if that folder is not writable.

### Environment variables (for troubleshooting)

| Variable | Effect |
|---|---|
| `PICASA_MOD_DPI=0` | Leaves the viewer DPI-unaware, with the system's (blurry) scaling as in the original. |
| `PICASA_MOD_NOHW` | Video decoding without hardware acceleration. |
| `PICASA_MOD_MUTE` | Plays videos without sound. |

### Limitations

- The viewer is a 32-bit program and can use at most about 2 GB of memory, which can limit very large images or long high-resolution videos.
- Videos have no seek bar; they play and loop.

### Building from source

Requirements:
- Visual Studio 2022 (C++ workload), CMake, Ninja;
- `vcpkg` next to this repository;
- the original install `picasa3-64.3.9.0` next to this repository, used as the source of `dist` and only ever read.

```bash
tools\deps.cmd
```

```bash
tools\build.cmd
```

`tools\build.cmd` builds `build\mod\version.dll` and assembles the original install plus the mod in `dist\` for local testing.

```bash
tools\release.cmd
```

`tools\release.cmd` builds, then packs `version.dll` and this file into `release\picasa-viewer-mod-<version>.zip`. The package contains no Google files.

GitHub Actions builds every push; the zip is under Artifacts on the run's page. Pushing a `v*` tag also publishes a release for it.

### Source layout

| Path | Contents |
|---|---|
| `mod/src/version_proxy.cpp` | Forwards the exports of the system `version.dll` |
| `mod/src/dllmain.cpp` | Installs the hooks in order |
| `mod/src/filetypes.cpp` | Extension-to-type mapping, Open dialog filter, file association table |
| `mod/src/decoder.cpp`, `mod/src/codecs/` | Decoders (WebP, HEIF, AVIF, JXL, GIF, WIC, FFmpeg video, WASAPI audio) and color management |
| `mod/src/animator.cpp` | Frame-by-frame display of animations and videos |
| `mod/src/dpi.cpp`, `mod/src/uiscale.cpp` | DPI awareness and interface scaling |
| `mod/src/respack.cpp` | Builds the modified resource pack copy at run time |
| `mod/src/settings.cpp` | The new settings window and file associations |
| `mod/src/picasa_features.cpp` | Removes the Picasa features; turns "More" into the settings button |
| `mod/src/network.cpp` | Blocks network access |

Picasa is Google software. This project neither contains nor distributes any Picasa files.
