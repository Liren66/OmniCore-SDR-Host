# Linux 与 macOS 安装

Linux 与 macOS 版提供与 Windows 相同的 `och` 命令行、C ABI 1 和 C++ 包装，底层共用 libusb 1.0 异步传输（`src/transport/posix`）。Windows 见 [Windows 安装](../../packaging/windows/INSTALL_WINDOWS.zh-CN.txt)。

| 平台 | 发布包 | 已验证环境 |
|---|---|---|
| Linux x86_64 | `OmniCoreHost-<版本>-linux-x86_64.tar.gz`，在 Ubuntu 22.04 上构建，glibc 不低于它的发行版都能运行；运行时需要系统的 `libusb-1.0-0` | Ubuntu 24.04.3、GCC 13、libusb 1.0.27、USB 3.0 5 Gbit/s。没有验证 Linux ARM64 和 Linux 下的 USB2 性能 |
| macOS 12+ | `OmniCoreHost-<版本>-macos-universal.tar.gz`，arm64 + x86_64 通用二进制，libusb 静态链接，不需要 Homebrew | Apple Silicon、USB 3.0。Intel Mac 没有实机测试 |

性能与具体 USB 端口、线材有关，见 [测试结果](benchmarks.md)。

## 安装发布包

两个平台都推荐装到 `/opt/omnicore/<版本>`，再把 `bin/och` 软链接到 `/usr/local/bin`。程序用相对 RPATH 找到 `lib/` 下的库，不需要设置 `LD_LIBRARY_PATH`，但要保持 `bin/` 与 `lib/` 的相对位置和库的软链接。README、SDK 与文档也在这个目录里。

**Linux**：

```bash
tar -xzf OmniCoreHost-0.8.0-linux-x86_64.tar.gz
sudo mkdir -p /opt/omnicore && sudo mv OmniCoreHost-linux /opt/omnicore/0.8.0
sudo ln -sfn /opt/omnicore/0.8.0/bin/och /usr/local/bin/och
sudo install -m 0644 /opt/omnicore/0.8.0/share/omnicore/60-omnicore.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
```

**macOS**（不需要驱动或 udev 规则）：

```bash
tar -xzf OmniCoreHost-0.8.0-macos-universal.tar.gz
xattr -dr com.apple.quarantine OmniCoreHost-macos   # 浏览器下载的归档会被 Gatekeeper 隔离
sudo mkdir -p /opt/omnicore && sudo mv OmniCoreHost-macos /opt/omnicore/0.8.0
sudo ln -sfn /opt/omnicore/0.8.0/bin/och /usr/local/bin/och
```

之后在任意目录运行 `och version`、`och info`。应用本身不需要 sudo。

## 从源码构建

**Linux**：

```bash
sudo apt-get install build-essential cmake ninja-build pkg-config libusb-1.0-0-dev
bash tools/build-linux.sh                      # 安装到 dist/OmniCoreHost-linux
./dist/OmniCoreHost-linux/bin/och info
```

脚本优先使用 `/usr/bin` 下的工具，避免其他 SDK（如 Xilinx 工具链）自带的旧版 CMake 抢占 PATH；`OCH_JOBS` 控制编译并发，`OCH_BUILD_DIR`、`OCH_INSTALL_DIR` 指定输出目录。脚本不修改系统电源策略、固件或内核 USB 内存参数。也可以手动构建并安装：

```bash
/usr/bin/cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release
/usr/bin/cmake --build build/linux -j4
sudo /usr/bin/cmake --install build/linux --prefix /opt/omnicore/0.8.0
```

**macOS**：

```bash
brew install cmake ninja pkg-config libusb
cmake -S . -B build/macos -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/macos
cmake --install build/macos --prefix dist/OmniCoreHost-macos
```

本机这样编译时动态链接 Homebrew 的 libusb；发布包由 `.github/workflows/release.yml` 静态链接 libusb 并编成通用二进制。

## Linux：普通用户访问 USB

若 `och info` 报 `LIBUSB_ERROR_ACCESS`，安装只匹配 `1a86:5537` 的 udev 规则（源码中为 `packaging/linux/60-omnicore.rules`，发布包中为 `share/omnicore/60-omnicore.rules`）：

```bash
sudo install -m 0644 packaging/linux/60-omnicore.rules /etc/udev/rules.d/60-omnicore.rules
sudo udevadm control --reload-rules
sudo usermod -aG plugdev "$USER"
```

重新登录以更新组成员身份，然后重新插拔设备。桌面会话通过 uaccess ACL 访问，SSH 用户使用 plugdev 组。检查动态库解析时用 `ldd "$(readlink -f /usr/local/bin/och)"`，让 ldd 按真实路径解析相对 RPATH。

## 运行与性能参数

```bash
och find --json
och info --json
och bench --rx --channels 1,2 --sample-rate 61.44M --seconds 60 --json
och bench --rx --tx --iq-bits 8 --channels 1,2 --sample-rate 61.44M \
  --seconds 60 --amplitude 0 --attenuation 89.75 --json
```

TX 吞吐测试发送全零 IQ，并设最大衰减 89.75 dB；不测射频功率、EVM 或接收灵敏度。普通 `bench` 返回 0 只代表命令完成，统计是否达标要看 JSON 的 `stability.pass`。Ctrl+C / SIGTERM 会清理流并返回中断码 130。文件接收、回放与发射音示例见 [README](../../README.zh-CN.md#快速上手)。

| 项目 | 说明 |
|---|---|
| USB 队列 | 默认每方向 32 × 196608 B，在 Linux 默认的 `usbfs_memory_mb=16` 下即可满速。只有增大 `--queue` / `--block` 后出现 `LIBUSB_ERROR_NO_MEM`，才需要评估调高 `/sys/module/usbcore/parameters/usbfs_memory_mb`；程序不会改动这个全局配置 |
| 主机缓冲 | `--buffer-mib` 调整用户态环形缓冲（默认 64 MiB），与内核 USB 队列预算无关 |
| macOS 睡眠 | 流运行期间程序会阻止系统空闲睡眠 |
| macOS 双通道满速 | 双通道 61.44 MSPS 接收在测试用的 Mac 上偶有设备端溢出；需要零溢出时先用 `och bench --json` 确认 |

出现 `LIBUSB_ERROR_IO`、EPROTO 或持续 RX 溢出时，先记录 `lsusb -t`（macOS 为 `system_profiler SPUSBDataType`）、端口、固件及原始统计，换一个 USB3 接口或线材后复测。标称 5 Gbit/s 不代表连接能稳定收发；有溢出但平均速率接近目标的结果不能判为合格。

## SoapySDR 插件与 MATLAB 工具箱

两者都是可选组件，发布包里不带插件；MATLAB 工具箱另有 zip。编译与使用见 [SoapySDR 插件](soapy.md) 和 [MATLAB 工具箱](matlab.md)。Linux 上插件装到 `/usr/local/lib/SoapySDR/modules<版本>/`，发行版自带的 SoapySDR 不一定搜索这个目录，找不到插件时设置 `SOAPY_SDR_PLUGIN_PATH`。USB 权限沿用上面的 udev 规则。

## SDK 集成

```cmake
find_package(OmniCoreHost 0.8 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE OmniCore::omnicore)
```

把安装目录加入 `CMAKE_PREFIX_PATH`。共享库为 Linux 的 `libomnicore.so.1`、macOS 的 `libomnicore.1.dylib`（ABI 1）。Linux 还需要系统的 libusb 1.0 和相容的 glibc / libstdc++。SDK 公共接口不含平台句柄。

## 后端实现要点

后端使用预排队的异步请求和独立的 libusb 事件线程；Linux 上支持时使用 DMA 映射缓冲，不支持时自动改用普通缓冲。控制读取保持挂起，关闭时取消，并阻止关闭期间重新提交；取消的请求要等回调完成后才释放内存。设备独占靠进程锁加接口 claim，进程异常退出后锁自动释放。
