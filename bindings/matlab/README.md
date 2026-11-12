# OmniCoreSDR for MATLAB

MATLAB R2018a 及以后版本直接使用 OmniCoreSDR：采集、发射、定时收发、全双工、System object 与 Simulink。不依赖任何工具箱。完整说明见 OmniCore Host 文档 `docs/matlab.md`。

## 安装（发布包）

1. 解压 `omnicore-matlab-<版本>-<平台>.zip`，得到文件夹 `omnicore-matlab`：MEX 文件、`+omnicore` 包、OmniCore Host 运行库、`examples`。
2. 在 MATLAB 中：

   ```matlab
   addpath('<解压目录>/omnicore-matlab');   % savepath 可以永久保存
   omnicore.version()
   omnicore.findDevices()
   ```

- **Windows**：先用 OmniCore Host 安装包装好 USB 驱动（`och-driver install`）。安装包勾选 “MATLAB” 组件时，文件夹在 `C:\Program Files\OmniCore Host\matlab`。
- **macOS**：下载的 zip 会被 Gatekeeper 隔离，解压后运行一次 `xattr -dr com.apple.quarantine omnicore-matlab`。
- **Linux**：需要 udev 规则（`packaging/linux/60-omnicore.rules`），见 `docs/INSTALL_LINUX.md`。

## 最短示例

```matlab
dev = omnicore.Device('SampleRate', 30.72e6, 'RxFrequency', 2.45e9);
[x, meta] = receive(dev, 1e6);        % 1e6 x 2 complex single
delete(dev)
```

更多见 `examples/`：`rx_spectrum.m`、`tx_tone_repeat.m`、`duplex_system_objects.m`、`timed_burst_loopback.m`、`simulink_duplex_example.m`。

## 从源码编译

- CMake：`cmake -S . -B build -DOCH_BUILD_MATLAB=ON [-DMatlab_ROOT_DIR=<MATLAB 目录>]`，构建后 `build/bindings/matlab/toolbox` 可直接 `addpath`；`cmake --build build --target omnicore_matlab_zip` 生成发布包。
- 只有 MATLAB 和已安装的 OmniCore Host：`build_omnicore_mex('<OCH 安装目录>')`（需要 `mex -setup` 配好的 C++ 编译器）。
