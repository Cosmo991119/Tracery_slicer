# Tracery Slicer 1.0.0

Tracery Slicer 的首个版本，基于 Snapmaker Orca，加入面向多色 FDM 打印的花窗工具。

- 普通模式与贯穿模式，支持正反面对应的花窗图案。
- 矢量绘制、填充和擦除工具，以及可调碎片密度。
- CMYW 自动配色与混色配方，优先选择双色方案。
- 花窗项目保存与恢复、缩略图修复和交互性能改进。

This first Tracery Slicer release builds on Snapmaker Orca and adds stained-glass tools for multicolor FDM printing: Normal and Through modes, vector painting/filling/erasing, adjustable fragment density, CMYW color recipes with a preference for two-color mixes, project persistence, and preview and performance improvements.

## Downloads / 下载

| Platform | Package | Requirements |
| --- | --- | --- |
| Windows x64 | `Windows-x64_Installer.exe` or `Windows-x64_Portable.zip` | Windows 10/11, Visual C++ runtime and WebView2 |
| macOS Apple Silicon | `macOS-arm64.dmg` | macOS 12 or later |
| macOS Intel | `macOS-x86_64.dmg` | macOS 12 or later |
| Linux x64 | `Linux-x86_64.AppImage` | Ubuntu 24.04 or a compatible system with GTK3 and WebKitGTK 4.1 |

Windows 便携版请完整解压后运行 `snapmaker-orca.exe`。macOS 将应用拖入 Applications。Linux 执行 `chmod +x Tracery_Slicer_*.AppImage` 后运行；Ubuntu 24.04 可能需要 `sudo apt install libfuse2t64 libwebkit2gtk-4.1-0`。

Extract the entire Windows portable ZIP before running `snapmaker-orca.exe`. Drag the macOS app into Applications. On Linux, make the AppImage executable; FUSE 2 and WebKitGTK 4.1 may need to be installed separately.

## Validation and limitations / 验证与限制

Packages are built by GitHub Actions and checked for command-line startup before publication. This does not replace interactive GUI or physical printer testing. Stained-glass mixing and printed results are still being refined; inspect the sliced preview before printing.

安装包由 GitHub Actions 构建并进行命令行启动检查，尚不能替代各平台的完整图形界面和实机打印验证。混色与打印效果仍在完善，请打印前检查切片预览。

The Windows packages are not signed with a commercial code-signing certificate. macOS packages use ad-hoc signatures and are not Apple-notarized; macOS may require opening the app through System Settings → Privacy & Security → Open Anyway after a blocked launch. Existing Snapmaker Orca application identifiers and configuration paths are retained; back up existing presets and projects before installing, and use the Windows portable package if you want to avoid replacing the installed upstream app.

Windows 安装包没有商业代码签名；macOS 使用临时签名，未经 Apple 公证，首次启动被阻止后可能需要在「系统设置 → 隐私与安全性」中选择「仍要打开」。目前仍沿用 Snapmaker Orca 的应用标识和配置路径，安装前请备份已有预设和项目；Windows 若希望避免替换已安装的上游应用，请使用便携版。

`SHA256SUMS.txt` contains SHA-256 checksums for all five packages. Source code and inherited third-party license notices are available in this repository under AGPL-3.0 and the respective component licenses.
