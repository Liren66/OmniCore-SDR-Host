# Building from Source

## Requirements

CMake 3.20+, Ninja, and a C++17 x64 toolchain (on Windows, MSYS2 MinGW-w64, default `C:\msys64\mingw64\bin`). Linux and macOS also need libusb 1.0; see [Installing on Linux and macOS](install.md#building-from-source) for the packages.

## Building and packaging

```powershell
# Windows: build and install to build/stage
powershell -File tools/build.ps1 -Configuration Release

# Create the installer dist/OmniCoreHost-<version>-win64-setup.exe and its .sha256
powershell -NoProfile -File packaging/windows/build-package.ps1 -StageDir build/stage
```

On Linux use `bash tools/build-linux.sh` (installs to `dist/OmniCoreHost-linux`); on macOS use CMake + Ninja. For dependencies and steps see [Installing on Linux and macOS](install.md#building-from-source). Release packages for all three platforms are produced by `.github/workflows/release.yml`.

When building from source, the examples are built by default (turn them off with `-DOCH_BUILD_EXAMPLES=OFF`); the executables are in `examples/` under the build directory (on Windows, next to `omnicore.dll`).

| Option | Builds | Notes |
|---|---|---|
| `-DOCH_BUILD_SOAPY=ON` | SoapySDR plugin | Requires the SoapySDR development package. Off by default; `tools/build.ps1`, `tools/build-linux.sh`, and the installer do not include it. See [SoapySDR Plugin](soapy.md) |
| `-DOCH_BUILD_MATLAB=ON` | MATLAB toolbox | Requires MATLAB R2018a+. `--target omnicore_matlab_zip` produces the release zip; the release pipeline makes one per platform and puts the Windows one into the installer's optional MATLAB task. See [MATLAB Toolbox](matlab.md) |
| `-DOCH_BUILD_EXAMPLES=OFF` | — | Skip the C++ examples |

The packaging script verifies the stage contents and version number, adds the driver helper, libusbK, and licenses, generates MSVC import libraries (needs `lib.exe` from VS Build Tools), and writes `PAYLOAD_SHA256.txt`. Inno Setup 6.2.2 is included in the repository; see [Building the Installer](../packaging/windows/BUILD_INSTALLER.txt).

## Documentation site

This site is built from `docs/` with MkDocs Material (`mkdocs.yml`) and published to GitHub Pages by `.github/workflows/docs.yml` when `main` changes. Every page exists in English (`docs/`) and Chinese (`docs/zh-CN/`); diagrams have a Chinese copy in `docs/images/zh-CN/`.

```bash
pip install -r docs/requirements.txt
mkdocs serve                 # live preview at http://127.0.0.1:8000/OmniCore-SDR-Host/
mkdocs build --strict        # what CI runs: broken links, anchors or images fail
```

## Project layout

```text
OmniCoreHost/
├── include/omnicore/   public C ABI (och.h) and C++ wrapper (och.hpp)
├── apps/
│   └── och/            och command line
├── src/
│   ├── capi/           public C interface entry points (extern "C", one file per module)
│   ├── core/           device, configuration transactions, stream threads, RF and board control
│   ├── dsp/            IQ codecs
│   ├── protocol/       16 KiB framed block parsing and generation
│   └── transport/      transport layer: win/ libusbK (Windows), posix/ libusb (Linux, macOS), fake/ offline tests
├── bindings/
│   ├── common/         mapping, interleaving, RX time, and TX burst logic shared by the plugins, with offline tests
│   ├── matlab/         MATLAB toolbox: MEX gateway (mex/), +omnicore package, reference docs (doc/), examples, tests, and hardware acceptance scripts
│   └── soapy/          SoapySDR plugin (driver=omnicore) and hardware acceptance scripts (hwtest/)
├── examples/           C++ examples, sweep waterfall (sweep, with Python plotting and a web demo), FTR example, GNU Radio flowgraphs (soapy/)
├── tests/              offline tests (frame parsing, FakeLink transport, multi-board sync orchestration) and on-board hardware timestamp tests
├── docs/               user and developer documentation (English; Chinese in docs/zh-CN/), images
├── cmake/              CMake package config templates
├── tools/              build scripts (including a cross-build script that checks the Windows build on macOS / Linux),
│                       throughput sweep (bench_sweep.py), documentation site hooks and theme overrides (mkdocs/)
├── packaging/
│   ├── windows/        Inno installer script, MSVC import library generation, driver helper source (driver/)
│   └── linux/          udev rules
└── third_party/        libusbK / libwdi and their licenses, Inno Setup compiler
```
