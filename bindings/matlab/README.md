# OmniCoreSDR for MATLAB

Use OmniCoreSDR directly from MATLAB R2018a and later: capture, transmit, timed RX/TX, full duplex, System objects, and Simulink. No toolboxes are required. See `docs/matlab.md` in the OmniCore Host documentation for the full guide.

## Installation (release package)

1. Extract `omnicore-matlab-<version>-<platform>.zip` to get the `omnicore-matlab` folder: the MEX file, the `+omnicore` package, the OmniCore Host runtime libraries, `examples`, and `doc`.
2. In MATLAB:

   ```matlab
   addpath('<extract-dir>/omnicore-matlab');   % use savepath to keep it permanently
   omnicore.version()
   omnicore.findDevices()
   ```

- **Windows**: install the USB driver first with the OmniCore Host installer (`och-driver install`). If you select the "MATLAB" component in the installer, the folder is at `C:\Program Files\OmniCore Host\matlab`.
- **macOS**: Gatekeeper quarantines downloaded zips; after extracting, run `xattr -dr com.apple.quarantine omnicore-matlab` once.
- **Linux**: requires the udev rule (`packaging/linux/60-omnicore.rules`); see `docs/install.md` in the OmniCore Host documentation.

## Minimal example

```matlab
dev = omnicore.Device('SampleRate', 30.72e6, 'RxFrequency', 2.45e9);
[x, meta] = receive(dev, 1e6);        % 1e6 x 2 complex single
delete(dev)
```

## Help documentation

`doc/` contains reference pages in MATLAB help format: syntax, description, examples, and arguments for every class, function, and method, plus topics on sample formats, full duplex, hardware time, Simulink, and error codes.

```matlab
omnicore.doc                  % start page
omnicore.doc('receive')       % a specific function or topic
```

When the folder is on the path, the pages also appear under "Supplemental Software" on the Help browser start page.

## Examples

Application examples in `examples/` (see `help fm_radio` and so on; `'Simulate', true` runs them without a device):

- `fm_radio`: FM broadcast radio with live playback and a spectrum display.
- `fm_loopback`: FM transmit-and-receive loopback measuring end-to-end latency and audio SNR.
- `video_link`: JPEG video frames sent over QPSK bursts, showing the constellation, EVM, and frequency offset.
- `sdr_scope`: simple triggered oscilloscope and spectrum analyzer.

Basic examples: `rx_spectrum.m`, `tx_tone_repeat.m`, `duplex_system_objects.m`, `timed_burst_loopback.m`, `simulink_duplex_example.m`.

## Building from source

- CMake: `cmake -S . -B build -DOCH_BUILD_MATLAB=ON [-DMatlab_ROOT_DIR=<MATLAB-dir>]`. After building, you can `addpath` `build/bindings/matlab/toolbox` directly; `cmake --build build --target omnicore_matlab_zip` creates the release package.
- With only MATLAB and an installed OmniCore Host: `build_omnicore_mex('<OCH-install-dir>')` (needs a C++ compiler configured with `mex -setup`).
