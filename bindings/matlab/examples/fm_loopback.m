function result = fm_loopback(varargin)
%FM_LOOPBACK Full-duplex FM link on one board: TX1 sends audio, RX1 receives and plays it.
%   fm_loopback() FM-modulates MATLAB's handel recording onto TX1 at 433.92 MHz and, in the same
%   loop, receives RX1, demodulates and plays it. At the end it lines the received audio up with
%   the sent audio and reports the end-to-end delay and the audio SNR.
%   RESULT = fm_loopback(...) returns a struct: Audio (received, 48 kHz), Reference (sent, the
%   same instants), AudioRate, DelaySeconds, SnrDb, Underflows (TX), Lost (RX samples).
%
%   Connect TX1 to RX1 through 30-60 dB of attenuation, or put antennas on both. The default TX
%   gain (30 dB, i.e. 59.75 dB attenuation) is far below 1 mW; raise it with care and only
%   within your local rules. 433.92 MHz is a short-range-device frequency; over the air at
%   2.4 GHz, Wi-Fi bursts disturb the link (see video_link).
%
%   fm_loopback(Name, Value) with
%     'Frequency'   TX carrier, Hz, default 433.92e6
%     'Source'      'handel' (default), an audio file name (audioread), or a vector
%     'SourceRate'  sample rate of a vector Source, Hz
%     'Seconds'     default: the length of the source
%     'TxGain'      dB, 0..89.75, default 30
%     'RxGain'      dB, or [] (default) for the device's slow AGC
%     'Offset'      the RX LO sits this far below the TX carrier, Hz, default 250e3
%     'Audio', 'Plot', 'Serial', 'Simulate'   as for fm_radio
%     'Channel'     simulation only: struct with Delay (samples), Loss (dB), NoiseDb (dBFS) and
%                   FrequencyOffset (Hz); default 3000 samples, 30 dB, -75 dBFS, 0 Hz
%
%   What it shows:
%     - full duplex in one MATLAB loop: each 40 ms block is modulated and written to the TX
%       stream, then 40 ms are read from the RX stream. read waits for the device, so the loop
%       runs at exactly the sample rate; 0.2 s of zeros written first give TX a margin
%       against MATLAB pauses (as omnicore.Transmitter's Latency does);
%     - offset tuning: TX and RX LOs 250 kHz apart, so TX LO leakage, RX DC and the wanted
%       signal do not overlap;
%     - measuring a link: the delay comes from the cross-correlation of sent and received
%       audio, the SNR from what is left after removing the best-fitting copy of the sent audio.
%
%   Example:
%       r = fm_loopback('Seconds', 5, 'Audio', false);
%       fprintf('delay %.1f ms, SNR %.1f dB\n', 1e3 * r.DelaySeconds, r.SnrDb);
%
%   See also fm_radio, omnicore.TxStream, omnicore.RxStream.

opt = struct('Frequency', 433.92e6, 'Source', 'handel', 'SourceRate', [], 'Seconds', [], ...
             'TxGain', 30, 'RxGain', [], 'Offset', 250e3, 'Audio', true, 'Plot', true, ...
             'Serial', '', 'Simulate', false, 'Channel', struct());
opt = parse_options(opt, varargin);
[src, fa] = load_source(opt.Source, opt.SourceRate);
if isempty(opt.Seconds)
    opt.Seconds = numel(src) / fa;
end

fs = 3.072e6;
block = 122880;                                   % 40 ms
link = sdr_link(struct('Simulate', opt.Simulate, 'Serial', opt.Serial, 'SampleRate', fs, ...
                       'RxFrequency', opt.Frequency - opt.Offset, 'TxFrequency', opt.Frequency, ...
                       'RxGain', opt.RxGain, 'TxGain', opt.TxGain, 'Tx', true, 'Bandwidth', 1.5e6, ...
                       'Pace', opt.Simulate && (opt.Audio || opt.Plot), 'Channel', opt.Channel));
cleanup = onCleanup(@() link.close());
fs = link.get('SampleRate');
tx = fm_transmitter(src, fa, fs);
rx = fm_receiver(fs, opt.Offset, 50e-6);
sink = audio_sink(rx.fsAudio, opt.Audio);
closeSink = onCleanup(@() sink.close());

view = [];
if opt.Plot
    view = make_view(opt.Frequency, opt.Offset, fs);
    drawnow;            % the first render takes ~1 s: do it before TX starts, or TX underflows
end
link.start();                                     % RX first: starting it takes a while
lead = round(0.2 * fs);
link.write(complex(zeros(lead, 1)), []);         % TX margin, see the help
blocks = ceil(opt.Seconds * fs / block);
audio = zeros(ceil(blocks * block / fs * rx.fsAudio) + 64, 1);
filled = 0;
lost = 0;
underflows = 0;
for k = 1:blocks
    [x, tx] = fm_modulate(tx, block);
    link.write(x, []);
    [y, meta] = link.read(block);
    lost = lost + meta.Lost;
    [a, rx] = fm_demodulate(y, rx);
    sink.push(0.8 * a);
    audio(filled + (1:numel(a))) = a;
    filled = filled + numel(a);
    if mod(k, 25) == 0
        s = link.txStatus();
        underflows = underflows + s.Underflow;
    end
    if ~isempty(view)
        if ~ishghandle(view.fig)
            break
        end
        if mod(k, 5) == 1
            update_view(view, y, a, fs, lost, underflows);
        end
    end
end
s = link.txStatus();
underflows = underflows + s.Underflow;
audio = audio(1:filled);

% Sent audio (without pre-emphasis, as the listener should hear it) at the received audio's
% instants, the source looping; then line the two up.
fo = rx.fsAudio;
ref = loop_source(src - mean(src), fa, (0:filled - 1).' / fo);
[delay, gain, snr] = align(audio, ref);
result = struct('Audio', audio, 'Reference', ref, 'AudioRate', fo, 'DelaySeconds', delay / fo, ...
                'SnrDb', snr, 'Underflows', underflows, 'Lost', lost);
fprintf(['fm_loopback: %.1f s, delay %.1f ms (including the %.0f ms TX margin), audio SNR %.1f dB, ' ...
         'gain %.2f, %d TX underflows, %d RX samples lost\n'], filled / fo, 1e3 * delay / fo, ...
        1e3 * lead / fs, snr, gain, underflows, lost);
if ~isempty(view) && ishghandle(view.fig)
    show_alignment(view, audio, ref, delay, gain, snr, fo);
end
if nargout == 0
    clear result
end
end

function [src, fa] = load_source(source, rate)
if ischar(source) || isstring(source)
    if strcmpi(source, 'handel')
        s = load('handel.mat');
        src = s.y;
        fa = s.Fs;
    else
        [src, fa] = audioread(char(source));
    end
else
    if isempty(rate)
        error('omnicore:InvalidArgument', 'A vector Source needs SourceRate');
    end
    src = source;
    fa = rate;
end
src = mean(double(src), 2);                       % mono
end

function y = loop_source(src, fa, t)
u = t * fa;
n = numel(src);
i0 = floor(u);
fr = u - i0;
y = src(mod(i0, n) + 1) .* (1 - fr) + src(mod(i0 + 1, n) + 1) .* fr;
end

function [lag, gain, snr] = align(a, ref)
% Delay LAG (samples) of A against REF by FFT cross-correlation, then the gain and SNR of the
% best fit over the overlap.
n = 2^nextpow2(2 * numel(a));
r = real(ifft(fft(a, n) .* conj(fft(ref, n))));  % r(L + 1) = sum a(m) ref(m - L)
maxLag = min(numel(a) - 1, round(numel(a) / 2));
[~, i] = max(abs(r(1:maxLag + 1)));
lag = i - 1;
x = ref(1:numel(a) - lag);
y = a(lag + 1:end);
gain = (x' * y) / (x' * x);
snr = 10 * log10(sum((gain * x).^2) / sum((y - gain * x).^2));
end

function v = make_view(frequency, offset, fs)
v.fig = figure('Name', 'FM loopback', 'NumberTitle', 'off', 'Color', 'w');
v.rf = subplot(2, 1, 1);
v.rfLine = plot(v.rf, NaN, NaN);
xlabel(v.rf, 'MHz');
ylabel(v.rf, 'dBFS');
title(v.rf, 'RX1: the FM carrier 250 kHz above the RX LO');
v.lo = frequency - offset;
xlim(v.rf, (v.lo + [-fs, fs] / 2) / 1e6);
ylim(v.rf, [-120, 0]);
grid(v.rf, 'on');
v.au = subplot(2, 1, 2);
v.auLine = plot(v.au, NaN, NaN);
xlabel(v.au, 'ms');
ylim(v.au, [-1.2, 1.2]);
grid(v.au, 'on');
end

function update_view(v, y, a, fs, lost, underflows)
[p, f] = psd_db(y, fs, 4096);
set(v.rfLine, 'XData', (v.lo + f) / 1e6, 'YData', p);
n = min(numel(a), 2400);
set(v.auLine, 'XData', (0:n - 1) / 48, 'YData', a(end - n + 1:end));
title(v.au, sprintf('Received audio (RX lost %d samples, TX underflows %d)', lost, underflows));
drawnow limitrate
end

function show_alignment(v, audio, ref, delay, gain, snr, fo)
n = min(round(0.05 * fo), numel(audio) - delay);
mid = delay + round((numel(audio) - delay) / 2);
k = max(delay + 1, mid - n + 1):mid;
cla(v.au);
plot(v.au, (k - 1) / fo * 1e3, gain * ref(k - delay), (k - 1) / fo * 1e3, audio(k), '--');
legend(v.au, 'sent (delayed, scaled)', 'received');
xlabel(v.au, 'ms');
title(v.au, sprintf('Delay %.1f ms, audio SNR %.1f dB', 1e3 * delay / fo, snr));
drawnow
end
