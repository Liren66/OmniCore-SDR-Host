function audio = fm_radio(frequency, varargin)
%FM_RADIO Broadcast FM receiver: live audio, RF and multiplex (MPX) spectra.
%   fm_radio(98.7e6) tunes RX1 to the station at 98.7 MHz and plays it (mono) for 30 seconds.
%   Close the figure to stop earlier. AUDIO = fm_radio(...) also returns the audio at 48 kHz.
%   Connect an antenna for 88-108 MHz to RX1; a short wire works for strong stations.
%
%   fm_radio(FREQUENCY, Name, Value) with
%     'Seconds'     how long to listen, default 30
%     'Gain'        RX gain in dB, or [] (default) for the device's slow AGC
%     'Deemphasis'  50e-6 (default; China, Europe) or 75e-6 (Americas, Korea)
%     'Audio'       false: no sound (plots only)
%     'Plot'        false: no figure
%     'Serial'      device serial number, '' = the only connected device
%     'Simulate'    true: no device; two simulated stations, at FREQUENCY (MATLAB's handel
%                   recording with a stereo pilot) and 600 kHz above it (a 440 Hz tone)
%
%   How it works:
%     1. The RX LO sits 250 kHz below the station, so the station is away from the LO leakage
%        and DC correction at 0 Hz; a digital mixer moves it back to 0 Hz.
%     2. At 3.072 MSPS, a +-110 kHz channel filter and decimation by 8 leave 384 kHz.
%     3. The phase difference between samples is the instantaneous frequency: the multiplex
%        signal (mono audio, 19 kHz stereo pilot, L-R at 38 kHz, RDS at 57 kHz).
%     4. De-emphasis, a 15 kHz audio filter and decimation by 8 give 48 kHz mono audio.
%   It reads the RX stream in 40 ms blocks; processing takes a few ms of each, so playback keeps
%   up. Without Audio Toolbox or DSP System Toolbox (audioDeviceWriter), sound comes through
%   audioplayer in 0.5 s chunks with short gaps.
%
%   Example:
%       a = fm_radio(98.7e6, 'Seconds', 10);
%       sound(a, 48e3)                         % play the recording again
%
%   See also fm_loopback, omnicore.RxStream.

opt = struct('Seconds', 30, 'Gain', [], 'Deemphasis', 50e-6, 'Audio', true, 'Plot', true, ...
             'Serial', '', 'Simulate', false);
opt = parse_options(opt, varargin);

fs = 3.072e6;
offset = 250e3;                                   % station frequency - LO
block = 122880;                                   % 40 ms
stations = {};
if opt.Simulate
    stations = simulated_stations(frequency, fs);
end
link = sdr_link(struct('Simulate', opt.Simulate, 'Serial', opt.Serial, 'SampleRate', fs, ...
                       'RxFrequency', frequency - offset, 'RxGain', opt.Gain, 'Bandwidth', 1.5e6, ...
                       'Pace', opt.Simulate && (opt.Audio || opt.Plot), 'Source', @station_signal));
cleanup = onCleanup(@() link.close());
fs = link.get('SampleRate');
rx = fm_receiver(fs, offset, opt.Deemphasis);
sink = audio_sink(rx.fsAudio, opt.Audio);
closeSink = onCleanup(@() sink.close());

blocks = ceil(opt.Seconds * fs / block);
audio = zeros(ceil(blocks * block / fs * rx.fsAudio) + 64, 1);
filled = 0;
lost = 0;
view = [];
if opt.Plot
    view = make_view(frequency, fs, rx.fsMpx);
end
for k = 1:blocks
    [x, meta] = link.read(block);
    lost = lost + meta.Lost;
    [a, rx, mpx] = fm_demodulate(x, rx);
    sink.push(0.8 * a);
    audio(filled + (1:numel(a))) = a;
    filled = filled + numel(a);
    if ~isempty(view)
        if ~ishghandle(view.fig)
            break
        end
        if mod(k, 5) == 1                         % redraw 5 times a second
            update_view(view, x, mpx, a, fs, rx.fsMpx, lost);
        end
    end
end
audio = audio(1:filled);
if lost > 0
    fprintf('fm_radio: %d samples lost (MATLAB fell behind)\n', lost);
end
if nargout == 0
    clear audio
end

    function x = station_signal(k, fsIn, lo)
        % Simulated broadcast: the stations' FM signals at the RX LO.
        x = zeros(numel(k), 1);
        for s = 1:numel(stations)
            st = stations{s};
            st.tx.p.Offset = st.frequency - lo;
            [y, st.tx] = fm_modulate(st.tx, numel(k));
            x = x + st.level * y;
            stations{s} = st;
        end
        x = x * sqrt(fs / fsIn);                  % keep the levels if the rate was rounded
    end
end

function stations = simulated_stations(frequency, fs)
s = load('handel.mat');                           % ships with MATLAB: y, Fs = 8192
tone = sin(2 * pi * 440 * (0:7999).' / 8000);
stations = {struct('frequency', frequency, 'level', 0.3, ...
                   'tx', fm_transmitter(s.y, s.Fs, fs, 'Pilot', true, 'Amplitude', 1)), ...
            struct('frequency', frequency + 600e3, 'level', 0.05, ...
                   'tx', fm_transmitter(tone, 8000, fs, 'Amplitude', 1))};
end

function v = make_view(frequency, fs, fsMpx)
v.fig = figure('Name', sprintf('FM %.1f MHz', frequency / 1e6), 'NumberTitle', 'off', 'Color', 'w');
v.rf = subplot(3, 1, 1);
v.rfLine = plot(v.rf, NaN, NaN);
xlabel(v.rf, 'MHz');
ylabel(v.rf, 'dBFS');
title(v.rf, sprintf('RF around %.1f MHz (station marked)', frequency / 1e6));
xlim(v.rf, (frequency + [-fs, fs] / 2 - 250e3) / 1e6);
ylim(v.rf, [-120, 0]);
line(v.rf, frequency / 1e6 * [1 1], [-120 0], 'Color', [0.85 0.33 0.1], 'LineStyle', '--');
grid(v.rf, 'on');
v.mpx = subplot(3, 1, 2);
v.mpxLine = plot(v.mpx, NaN, NaN);
xlabel(v.mpx, 'kHz');
ylabel(v.mpx, 'dB');
title(v.mpx, 'Multiplex: mono 0-15 kHz, pilot 19 kHz, stereo 23-53 kHz, RDS 57 kHz');
xlim(v.mpx, [0, min(100, fsMpx / 2e3)]);
ylim(v.mpx, [-90, 10]);
grid(v.mpx, 'on');
v.au = subplot(3, 1, 3);
v.auLine = plot(v.au, NaN, NaN);
xlabel(v.au, 'ms');
title(v.au, 'Audio');
ylim(v.au, [-1.2, 1.2]);
grid(v.au, 'on');
v.lo = frequency - 250e3;
end

function update_view(v, x, mpx, a, fs, fsMpx, lost)
[p, f] = psd_db(x, fs, 4096);
set(v.rfLine, 'XData', (v.lo + f) / 1e6, 'YData', p);
[p, f] = psd_db(mpx, fsMpx, 4096, true);
set(v.mpxLine, 'XData', f / 1e3, 'YData', p);
n = min(numel(a), 2400);
set(v.auLine, 'XData', (0:n - 1) / 48, 'YData', a(end - n + 1:end));
title(v.au, sprintf('Audio (lost samples so far: %d)', lost));
drawnow limitrate
end
