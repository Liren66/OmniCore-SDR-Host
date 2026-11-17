# Installing on Linux and macOS

The Linux and macOS builds provide the same `och` command line, C ABI 1, and C++ wrapper as Windows, and share libusb 1.0 asynchronous transfers underneath (`src/transport/posix`). For Windows, see [Windows Installation](../packaging/windows/INSTALL_WINDOWS.txt).

| Platform | Release package | Verified environment |
|---|---|---|
| Linux x86_64 | `OmniCoreHost-<version>-linux-x86_64.tar.gz`, built on Ubuntu 22.04, runs on distributions with the same or newer glibc; needs the system `libusb-1.0-0` at runtime | Ubuntu 24.04.3, GCC 13, libusb 1.0.27, USB 3.0 5 Gbit/s. Linux ARM64 and USB2 performance on Linux have not been verified |
| macOS 12+ | `OmniCoreHost-<version>-macos-universal.tar.gz`, arm64 + x86_64 universal binary, libusb statically linked, no Homebrew needed | Apple Silicon, USB 3.0. Not tested on Intel Macs |

Performance depends on the specific USB port and cable; see [Benchmarks](benchmarks.md).

## Installing the release package

On both platforms we recommend installing to `/opt/omnicore/<version>` and symlinking `bin/och` into `/usr/local/bin`. The programs find the libraries under `lib/` through a relative RPATH, so `LD_LIBRARY_PATH` is not needed, but the relative position of `bin/` and `lib/` and the library symlinks must be kept. The README, SDK, and documentation are also in this directory.

**Linux**:

```bash
tar -xzf OmniCoreHost-0.8.0-linux-x86_64.tar.gz
sudo mkdir -p /opt/omnicore && sudo mv OmniCoreHost-linux /opt/omnicore/0.8.0
sudo ln -sfn /opt/omnicore/0.8.0/bin/och /usr/local/bin/och
sudo install -m 0644 /opt/omnicore/0.8.0/share/omnicore/60-omnicore.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
```

**macOS** (no driver or udev rule needed):

```bash
tar -xzf OmniCoreHost-0.8.0-macos-universal.tar.gz
xattr -dr com.apple.quarantine OmniCoreHost-macos   # Gatekeeper quarantines archives downloaded by a browser
sudo mkdir -p /opt/omnicore && sudo mv OmniCoreHost-macos /opt/omnicore/0.8.0
sudo ln -sfn /opt/omnicore/0.8.0/bin/och /usr/local/bin/och
```

Then run `och version` and `och info` from any directory. The application itself does not need sudo.

## Building from source

**Linux**:

```bash
sudo apt-get install build-essential cmake ninja-build pkg-config libusb-1.0-0-dev
bash tools/build-linux.sh                      # installs to dist/OmniCoreHost-linux
./dist/OmniCoreHost-linux/bin/och info
```

The script prefers the tools in `/usr/bin`, so that an old CMake shipped with another SDK (such as the Xilinx toolchain) does not take over PATH; `OCH_JOBS` controls build parallelism, and `OCH_BUILD_DIR` and `OCH_INSTALL_DIR` set the output directories. The script does not modify system power policy, firmware, or kernel USB memory parameters. You can also build and install by hand:

```bash
/usr/bin/cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release
/usr/bin/cmake --build build/linux -j4
sudo /usr/bin/cmake --install build/linux --prefix /opt/omnicore/0.8.0
```

**macOS**:

```bash
brew install cmake ninja pkg-config libusb
cmake -S . -B build/macos -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/macos
cmake --install build/macos --prefix dist/OmniCoreHost-macos
```

A local build like this links Homebrew's libusb dynamically; the release package is built by `.github/workflows/release.yml` with libusb statically linked as a universal binary.

## Linux: USB access for regular users

If `och info` reports `LIBUSB_ERROR_ACCESS`, install the udev rule that only matches `1a86:5537` (`packaging/linux/60-omnicore.rules` in the source tree, `share/omnicore/60-omnicore.rules` in the release package):

```bash
sudo install -m 0644 packaging/linux/60-omnicore.rules /etc/udev/rules.d/60-omnicore.rules
sudo udevadm control --reload-rules
sudo usermod -aG plugdev "$USER"
```

Log in again to update group membership, then replug the device. Desktop sessions get access through the uaccess ACL, and SSH users through the plugdev group. To check shared library resolution, use `ldd "$(readlink -f /usr/local/bin/och)"` so that ldd resolves the relative RPATH from the real path.

## Running and performance settings

```bash
och find --json
och info --json
och bench --rx --channels 1,2 --sample-rate 61.44M --seconds 60 --json
och bench --rx --tx --iq-bits 8 --channels 1,2 --sample-rate 61.44M \
  --seconds 60 --amplitude 0 --attenuation 89.75 --json
```

The TX throughput test sends all-zero IQ with the maximum attenuation of 89.75 dB; it does not measure RF power, EVM, or receive sensitivity. A plain `bench` returning 0 only means the command completed; whether the statistics pass is given by `stability.pass` in the JSON. Ctrl+C / SIGTERM clean up the streams and return the interrupt code 130. For file receive, playback, and tone transmit examples, see the [README](../README.md#quick-start).

| Item | Notes |
|---|---|
| USB queue | By default 32 × 196608 B per direction, which reaches full speed with Linux's default `usbfs_memory_mb=16`. Only if `LIBUSB_ERROR_NO_MEM` appears after increasing `--queue` / `--block` do you need to consider raising `/sys/module/usbcore/parameters/usbfs_memory_mb`; the program never changes this global setting |
| Host buffer | `--buffer-mib` sets the user-space ring buffer (default 64 MiB), independent of the kernel USB queue budget |
| macOS sleep | The program prevents idle system sleep while streams are running |
| macOS dual-channel full speed | Dual-channel 61.44 MSPS receive occasionally overflows on the device side on the test Mac; if you need zero overflows, confirm with `och bench --json` first |

When `LIBUSB_ERROR_IO`, EPROTO, or persistent RX overflows occur, first record `lsusb -t` (on macOS, `system_profiler SPUSBDataType`), the port, firmware, and raw statistics, then retest on a different USB3 port or cable. A nominal 5 Gbit/s does not mean the connection can stream reliably; a result with overflows is not a pass even if the average rate is close to the target.

## SoapySDR plugin and MATLAB toolbox

Both are optional components: the release package does not include the plugin, and the MATLAB toolbox has its own zip. For building and usage, see [SoapySDR Plugin](soapy.md) and [MATLAB Toolbox](matlab.md). On Linux the plugin installs to `/usr/local/lib/SoapySDR/modules<version>/`; the SoapySDR shipped with your distribution may not search that directory, so set `SOAPY_SDR_PLUGIN_PATH` if the plugin is not found. USB permissions use the udev rule above.

## SDK integration

```cmake
find_package(OmniCoreHost 0.8 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE OmniCore::omnicore)
```

Add the install directory to `CMAKE_PREFIX_PATH`. The shared library is `libomnicore.so.1` on Linux and `libomnicore.1.dylib` on macOS (ABI 1). Linux also needs the system libusb 1.0 and a compatible glibc / libstdc++. The public SDK interface contains no platform handles.

## Backend implementation notes

The backend uses pre-queued asynchronous requests and a dedicated libusb event thread; on Linux it uses DMA-mapped buffers where supported and falls back to ordinary buffers otherwise. Control reads stay pending, are cancelled on close, and are prevented from being resubmitted during close; cancelled requests are only freed after their callbacks complete. Device exclusivity relies on a process lock plus an interface claim, and the lock is released automatically if the process exits abnormally.
