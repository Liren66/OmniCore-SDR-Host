# 从源码构建

## 环境要求

需要 CMake 3.20+、Ninja 与 C++17 x64 工具链（Windows 使用 MSYS2 MinGW-w64，默认 `C:\msys64\mingw64\bin`）。Linux 与 macOS 还需要 libusb 1.0，依赖包见 [Linux 与 macOS 安装](install.md#从源码构建)。

## 构建与打包

```powershell
# Windows：编译并安装到 build/stage
powershell -File tools/build.ps1 -Configuration Release

# 生成安装包 dist/OmniCoreHost-<版本>-win64-setup.exe 及 .sha256
powershell -NoProfile -File packaging/windows/build-package.ps1 -StageDir build/stage
```

Linux 用 `bash tools/build-linux.sh`（安装到 `dist/OmniCoreHost-linux`），macOS 用 CMake + Ninja，依赖与步骤见 [Linux 与 macOS 安装](install.md#从源码构建)。三个平台的发布包都由 `.github/workflows/release.yml` 生成。

从源码构建时示例默认一起编译（`-DOCH_BUILD_EXAMPLES=OFF` 可关闭），可执行文件在构建目录的 `examples/` 下（Windows 与 `omnicore.dll` 同目录）。

| 选项 | 构建内容 | 说明 |
|---|---|---|
| `-DOCH_BUILD_SOAPY=ON` | SoapySDR 插件 | 需要 SoapySDR 开发包。默认关闭，`tools/build.ps1`、`tools/build-linux.sh` 和安装包都不包含。见 [SoapySDR 插件](soapy.md) |
| `-DOCH_BUILD_MATLAB=ON` | MATLAB 工具箱 | 需要 MATLAB R2018a+。`--target omnicore_matlab_zip` 生成发布用的 zip；发布流水线为三个平台各生成一个，并把 Windows 版放进安装包的可选 MATLAB 任务。见 [MATLAB 工具箱](matlab.md) |
| `-DOCH_BUILD_EXAMPLES=OFF` | — | 不编译 C++ 示例 |

打包脚本会校验 stage 内容与版本号，加入驱动助手、libusbK 与许可证，生成 MSVC 导入库（需 VS Build Tools 的 `lib.exe`），并写入 `PAYLOAD_SHA256.txt`。Inno Setup 6.2.2 已随工程提供，详见 [安装包构建说明](../../packaging/windows/BUILD_INSTALLER.txt)。

## 文档站

本站由 `docs/` 经 MkDocs Material（`mkdocs.yml`）生成，`main` 有改动时由 `.github/workflows/docs.yml` 发布到 GitHub Pages。每一页都有英文（`docs/`）和中文（`docs/zh-CN/`）两版，插图的中文版在 `docs/images/zh-CN/`。

```bash
pip install -r docs/requirements.txt
mkdocs serve                 # 本地预览 http://127.0.0.1:8000/OmniCore-SDR-Host/
mkdocs build --strict        # 与 CI 相同：坏链、坏锚点或缺图都会失败
```

## 工程目录

```text
OmniCoreHost/
├── include/omnicore/   公共 C ABI (och.h) 与 C++ 包装 (och.hpp)
├── apps/
│   └── och/            och 命令行
├── src/
│   ├── capi/           公共 C 接口入口（extern "C"，按模块分文件）
│   ├── core/           设备、配置事务、流线程、射频与板控
│   ├── dsp/            IQ 编解码
│   ├── protocol/       16 KiB 成帧块解析与生成
│   └── transport/      传输层：win/ libusbK（Windows）、posix/ libusb（Linux、macOS）、fake/ 离线测试
├── bindings/
│   ├── common/         各插件共用的映射、交织、RX 时间与 TX 突发逻辑及其离线测试
│   ├── matlab/         MATLAB 工具箱：MEX 网关（mex/）、+omnicore 包、参考文档（doc/）、示例、测试与实机验收脚本
│   └── soapy/          SoapySDR 插件（driver=omnicore）与实机验收脚本（hwtest/）
├── examples/           C++ 示例、扫频瀑布图（sweep，含 Python 绘图与网页演示）、FTR 示例、GNU Radio 流图（soapy/）
├── tests/              离线测试（成帧解析、FakeLink 传输、多板同步编排）与硬件时间戳板上测试
├── docs/               使用与开发文档（英文；中文在 docs/zh-CN/）、插图
├── cmake/              CMake 包配置模板
├── tools/              构建脚本（含在 macOS / Linux 上检查 Windows 版能否编译的交叉编译脚本）、
│                       吞吐扫描（bench_sweep.py）、文档站钩子与主题覆盖（mkdocs/）
├── packaging/
│   ├── windows/        Inno 安装包脚本、MSVC 导入库生成、驱动助手源码（driver/）
│   └── linux/          udev 规则
└── third_party/        libusbK / libwdi 及其许可证、Inno Setup 编译器
```
