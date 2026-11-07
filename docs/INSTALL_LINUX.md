# Linux 使用与构建

原有 Windows libusbK 后端继续保留。Linux 版提供相同的 `och` 命令行、C ABI 1 和 C++ 包装，使用系统 libusb 1.0 异步传输。

## 已验证环境

Ubuntu 24.04.3 x86_64、GCC 13、libusb 1.0.27、USB 3.0 5 Gbit/s。
发布包在该系统构建，其他发行版宜从源码构建；没有验证 Linux ARM64 或 USB2 性能。
性能与具体 USB 端口/线材有关。

## 源码构建

```bash
sudo apt-get install build-essential cmake ninja-build pkg-config libusb-1.0-0-dev
bash tools/build-linux.sh
./dist/OmniCoreHost-linux/bin/och info
```

脚本使用系统 `/usr/bin` 工具，避免 Xilinx SDK 的旧版 CMake 抢占 PATH。
`OCH_JOBS` 可控制编译并发；`OCH_BUILD_DIR` 与 `OCH_INSTALL_DIR` 可指定输出目录。
脚本不修改系统电源策略、固件或内核 USB 内存参数。

手动构建：

```bash
/usr/bin/cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release
/usr/bin/cmake --build build/linux -j4
```

## 安装到系统路径

从工程目录完成上述构建后：

```bash
sudo /usr/bin/cmake --install build/linux --prefix /opt/omnicore/0.6.0
sudo ln -sfn /opt/omnicore/0.6.0/bin/och /usr/local/bin/och
cd /tmp
och version
och info
```

或者解压发行归档后，将整个 `OmniCoreHost-linux/` 目录复制到 `/opt/omnicore/0.6.0/`，再创建同样的软链接。必须保留 `bin/` 与 `lib/` 的相对布局以及库的软链接。无需设置 `LD_LIBRARY_PATH`，无需用 sudo 运行应用。若检查动态库路径，应使用 `ldd "$(readlink -f /usr/local/bin/och)"`，使检查工具按真实 ELF 路径解析相对 RPATH。

README、SDK 与文档同安装在 `/opt/omnicore/0.6.0/`。运行库依赖系统 `libusb-1.0-0`。

## 普通用户访问 USB

若 `och info` 报 `LIBUSB_ERROR_ACCESS`，安装仅匹配 `1a86:5537` 的规则：

```bash
sudo install -m 0644 packaging/60-omnicore.rules /etc/udev/rules.d/60-omnicore.rules
sudo udevadm control --reload-rules
sudo usermod -aG plugdev "$USER"
```

重新登录以更新组成员身份，然后重新插拔设备。桌面会话可通过 uaccess ACL 访问，SSH 用户使用 plugdev 组。应用本身不需要 sudo。发布包也在 `share/omnicore/60-omnicore.rules` 附带此规则。

## 收发示例

```bash
och find --json
och info --json
och bench --rx --channels 1,2 --sample-rate 61.44M --seconds 60 --json
och bench --rx --tx --iq-bits 8 --channels 1,2 --sample-rate 61.44M \
  --seconds 60 --amplitude 0 --attenuation 89.75 --json
```

TX 吞吐测试发送全零 IQ，并设最大衰减 89.75 dB；不测射频功率、EVM 或接收灵敏度。普通 `bench` 返回 0 只代表命令完成，统计是否达标应检查 JSON 的 `stability.pass`。Ctrl+C/SIGTERM 会清理流并返回中断码 130。文件接收、回放与发射音示例见 [README](../README.md#快速上手)。

默认队列为每方向 32 × 196608 B，已在 `usbfs_memory_mb=16` 下验证。只有增大 `--queue`/`--block` 后遇到 `LIBUSB_ERROR_NO_MEM` 时，才需要评估提高 `/sys/module/usbcore/parameters/usbfs_memory_mb`；程序不会自动更改全局配置。`--buffer-mib` 调整的是用户态环形缓冲，不等于内核 USB 队列预算。

出现 `LIBUSB_ERROR_IO`、EPROTO 或持续 RX 溢出时，应先记录 `lsusb -t`、端口、固件及原始统计，尝试另一 USB3 接口/线材后复测。标称 5 Gbit/s 本身不代表连接能稳定收发。不要将有溢出但平均速率接近目标的结果判为合格。

## SDK 集成与发布包

```cmake
find_package(OmniCoreHost CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE OmniCore::omnicore)
```

把发布包路径加入 `CMAKE_PREFIX_PATH`。共享库以 `libomnicore.so.1` 提供 ABI 1；发布包中的 `bin/och` 使用相对 RPATH，可移动整个目录运行，无需设置 `LD_LIBRARY_PATH`。系统需要 libusb 1.0 和相容的 glibc/libstdc++。SDK 公共接口没有 Windows 句柄。

Linux 后端使用预排队异步请求、独立 libusb 事件线程和支持时的 DMA 映射缓冲；不支持 DMA 映射时自动使用普通缓冲。控制读取保持挂起，关闭时取消，并阻止关闭期间重新提交；取消请求必须等回调完成后才能释放内存。设备独占使用进程锁和接口 claim，进程异常退出后锁自动释放。
