function result = video_link(varargin)
%VIDEO_LINK Digital video over the air on one board: JPEG frames in QPSK bursts, TX1 -> RX1.
%   video_link() reads MATLAB's xylophone.mp4 frame by frame, compresses each frame to JPEG,
%   sends it as one timed QPSK burst on TX1 at 433.92 MHz and decodes it from RX1, showing the
%   sent and received frames, the constellation and link statistics for 20 seconds. Close the
%   figure to stop earlier.
%   RESULT = video_link(...) returns a struct: Sent, Received (frames that passed the
%   checksum), Failed (struct of counts by reason), BitRate (on air, bit/s), Throughput
%   (received JPEG bits per second of running time), EvmDb and CfoHz (means over received
%   frames), DelaySamples (TX StartTime -> burst on RX) and LastFrame (the last received image).
%
%   Connect TX1 to RX1 through 30-60 dB of attenuation, or use antennas. The default TX gain
%   (40 dB, i.e. 49.75 dB attenuation) is far below 1 mW. Every bit of a frame must arrive
%   intact, so the link needs about 15 dB SNR. Over the air at 2.4 GHz, Wi-Fi frames (a few ms,
%   stronger than this signal) wipe out parts of many bursts; a cable, or 433.92 MHz, avoids
%   that.
%
%   video_link(Name, Value) with
%     'Frequency'   Hz, default 433.92e6 (TX and RX)
%     'Video'       a video file VideoReader can read, default 'xylophone.mp4'; '' or a file
%                   that cannot be read uses an animated test picture (peppers.png)
%     'Scale'       keep every Scale-th pixel in each direction, default 1
%     'Quality'     JPEG quality 1-100, default 50
%     'Seconds'     default 20
%     'SampleRate'  default 3.072e6; 4 samples per symbol, so 1.536 Mbit/s on air at the
%                   default, about 1 MHz wide
%     'TxGain'      dB, default 40
%     'RxGain'      dB, default 40; [] for the slow AGC (it may switch gain, and phase, inside a
%                   burst)
%     'Plot', 'Serial', 'Simulate'   as for fm_loopback
%     'Channel'     simulation only: struct with Delay (samples), Loss (dB), NoiseDb (dBFS) and
%                   FrequencyOffset (Hz); default 3000 samples, 30 dB, -75 dBFS, 300 Hz. For
%                   example struct('Loss', 68) shows checksum failures at low SNR
%
%   The burst: [preamble | header | payload], root-raised-cosine QPSK (roll-off 0.35):
%     preamble  a 63-symbol Zadoff-Chu sequence sent twice: found by correlation, its two
%               halves give the carrier frequency offset, the whole the phase and gain
%     header    16 bytes: 'OC', frame number, payload length, payload and header Adler-32
%     payload   the JPEG file, whitened (XOR with a fixed pseudo-random sequence), with a pilot
%               block of 16 known symbols before every 512 data symbols and at the end
%   A block of 16 known pilot symbols every 512 data symbols gives the receiver the phase and
%   gain along the burst (and the frequency offset the preamble estimate left), without the
%   slips that blind tracking suffers at low SNR. A frame whose checksum fails is dropped; the
%   previous one stays on screen.
%
%   Timing comes from the hardware clock: each burst is written with a StartTime 50 ms ahead,
%   and the receiver cuts exactly that span (plus the measured TX-to-RX delay) out of the RX
%   stream by its timestamps, so it never searches more than one burst's worth of samples.
%
%   Example:
%       r = video_link('Seconds', 10, 'Quality', 30);
%       fprintf('%d of %d frames, %.0f kbit/s\n', r.Received, r.Sent, r.Throughput / 1e3);
%
%   See also fm_loopback, timed_burst_loopback, omnicore.TxStream.

opt = struct('Frequency', 433.92e6, 'Video', 'xylophone.mp4', 'Scale', 1, 'Quality', 50, ...
             'Seconds', 20, 'SampleRate', 3.072e6, 'TxGain', 40, 'RxGain', 40, 'Plot', true, ...
             'Serial', '', 'Simulate', false, 'Channel', struct('FrequencyOffset', 300));
opt = parse_options(opt, varargin);

link = sdr_link(struct('Simulate', opt.Simulate, 'Serial', opt.Serial, 'SampleRate', opt.SampleRate, ...
                       'RxFrequency', opt.Frequency, 'TxFrequency', opt.Frequency, 'RxGain', opt.RxGain, ...
                       'TxGain', opt.TxGain, 'Tx', true, 'Pace', opt.Simulate && opt.Plot, ...
                       'Channel', opt.Channel));
cleanup = onCleanup(@() link.close());
fs = link.get('SampleRate');
m = modem(fs);
src = video_source(opt.Video, opt.Scale);
tmp = [tempname, '.jpg'];
cleanTmp = onCleanup(@() delete_if_exists(tmp));

lead = round(0.05 * fs);                          % StartTime this far ahead of the device clock
pre = 2000;                                       % samples kept before StartTime
delay = [];                                       % TX StartTime -> burst start on RX, learned
nextFree = 0;
view = [];
if opt.Plot
    view = make_view();
end
sent = 0;
received = 0;
failed = struct('NotFound', 0, 'Header', 0, 'Incomplete', 0, 'Checksum', 0, 'Decode', 0, 'Late', 0);
evm = [];
cfo = [];
bits = 0;
last = [];
clock = tic;
while toc(clock) < opt.Seconds
    img = src();
    payload = jpeg_encode(img, opt.Quality, tmp);
    x = tx_burst(m, mod(sent, 65536), payload);
    t = max(link.now() + lead, nextFree);
    link.write(x, t);
    sent = sent + 1;
    nextFree = t + numel(x) + 1000;
    if isempty(delay)
        post = round(0.05 * fs);                  % first burst: search 50 ms after it
    else
        post = delay + 2000;
    end
    w = link.window(t - pre, pre + numel(x) + post);
    [got, info] = rx_burst(m, w);
    if ~isempty(info.start)
        delay = max(0, info.start - 1 - pre);
    end
    reason = info.error;
    if isempty(reason)
        try
            last = jpeg_decode(got, tmp);
            received = received + 1;
            evm(end + 1) = info.evm;              %#ok<AGROW>
            cfo(end + 1) = info.cfo;              %#ok<AGROW>
            bits = bits + 8 * numel(got);
        catch
            reason = 'Decode';
        end
    end
    if ~isempty(reason)
        failed.(reason) = failed.(reason) + 1;
    end
    if mod(sent, 10) == 0
        s = link.txStatus();
        failed.Late = failed.Late + s.Late;
    end
    if ~isempty(view)
        if ~ishghandle(view.fig)
            break
        end
        update_view(view, img, last, info, sent, received, failed, bits / toc(clock), ...
                    2 * fs / m.sps, numel(payload));
    end
end
elapsed = toc(clock);
result = struct('Sent', sent, 'Received', received, 'Failed', failed, 'BitRate', 2 * fs / m.sps, ...
                'Throughput', bits / elapsed, 'EvmDb', mean(evm), 'CfoHz', mean(cfo), ...
                'DelaySamples', delay, 'LastFrame', last);
fprintf(['video_link: %d of %d frames received in %.1f s (%.1f frames/s), %.0f kbit/s of JPEG, ' ...
         'EVM %.1f dB, CFO %.0f Hz, TX->RX delay %d samples\n'], received, sent, elapsed, ...
        received / elapsed, result.Throughput / 1e3, result.EvmDb, result.CfoHz, delay);
if nargout == 0
    clear result
end
end

% --- modem ------------------------------------------------------------------------------------

function m = modem(fs)
m.sps = 4;
m.symbolRate = fs / m.sps;
m.rrc = rrc_pulse(0.35, 8, m.sps);
m.delay = numel(m.rrc) - 1;                       % TX pulse + matched filter, samples
n = (0:62).';
zc = exp(-1j * pi * 25 * n .* (n + 1) / 63);      % Zadoff-Chu, root 25: flat spectrum, sharp correlation
m.pre = [zc; zc];
m.headerBytes = 16;
m.segment = 512;                                  % data symbols between pilot blocks
m.pilot = exp(1j * pi / 4 * (2 * mod(floor((0:15).' * 2.6), 4) + 1));  % 16 known QPSK symbols
m.maxPayload = 2^20;
rs = RandStream('mt19937ar', 'Seed', 12345);
m.white = randi(rs, [0, 255], m.maxPayload, 1);
end

function h = rrc_pulse(beta, span, sps)
% Root-raised-cosine pulse, SPAN symbols long, unit energy.
t = (-span * sps / 2:span * sps / 2).' / sps;
h = zeros(size(t));
for i = 1:numel(t)
    ti = t(i);
    if ti == 0
        h(i) = 1 - beta + 4 * beta / pi;
    elseif abs(abs(ti) - 1 / (4 * beta)) < 1e-9
        h(i) = beta / sqrt(2) * ((1 + 2 / pi) * sin(pi / (4 * beta)) + (1 - 2 / pi) * cos(pi / (4 * beta)));
    else
        h(i) = (sin(pi * ti * (1 - beta)) + 4 * beta * ti * cos(pi * ti * (1 + beta))) / ...
               (pi * ti * (1 - (4 * beta * ti)^2));
    end
end
h = h / sqrt(sum(h.^2));
end

function x = tx_burst(m, seq, payload)
n = numel(payload);
if n > m.maxPayload
    error('omnicore:InvalidArgument', 'A frame of %d bytes is too large; lower Quality or raise Scale', n);
end
head = [79; 67; be(seq, 2); be(n, 4); be(adler32(payload), 4)];
head = [head; be(adler32(head), 4)];
data = bitxor(payload, m.white(1:n));
s = [m.pre; qpsk_map(head); with_pilots(m, qpsk_map(data)); zeros(8, 1)];
up = zeros(numel(s) * m.sps, 1);
up(1:m.sps:end) = s;
x = conv(up, m.rrc);
x = 0.7 * x / max(abs(x));
end

function y = with_pilots(m, d)
% Data symbols D with a pilot block before every m.segment of them and one at the end.
isPilot = pilot_layout(m, numel(d));
y = complex(zeros(numel(isPilot), 1));
y(~isPilot) = d;
y(isPilot) = repmat(m.pilot, sum(isPilot) / numel(m.pilot), 1);
end

function [isPilot, blockStart] = pilot_layout(m, nd)
% Positions of the pilot blocks among ND data symbols: [P D(segment) P D(segment) ... P].
P = numel(m.pilot);
nseg = ceil(nd / m.segment);
len = nd + P * (nseg + 1);
blockStart = (0:nseg).' * (P + m.segment) + 1;
blockStart(end) = len - P + 1;                    % the last block follows the (short) last segment
isPilot = false(len, 1);
for b = 1:numel(blockStart)
    isPilot(blockStart(b) + (0:P - 1)) = true;
end
end

function [payload, info] = rx_burst(m, w)
payload = [];
info = struct('start', [], 'error', 'NotFound', 'evm', NaN, 'cfo', NaN, 'symbols', []);
w = w - mean(w);                                  % TX LO leakage and RX DC sit at 0 Hz
z = filter(m.rrc, 1, w);
% Correlate with the preamble at symbol spacing; normalise by the energy at the same positions
% so the peak is 1 for a clean burst whatever its level.
L = numel(m.pre);
T = zeros((L - 1) * m.sps + 1, 1);
T(1:m.sps:end) = m.pre;
if numel(z) < numel(T)
    return
end
nfft = 2^nextpow2(numel(z) + numel(T));
c = ifft(fft(z, nfft) .* conj(fft(T, nfft)));
e = ifft(fft(abs(z).^2, nfft) .* conj(fft(double(T ~= 0), nfft)));
valid = numel(z) - numel(T) + 1;
metric = abs(c(1:valid)).^2 ./ (real(e(1:valid)) * sum(abs(m.pre).^2) + eps);
[peak, k] = max(metric);
if peak < 0.3
    return
end
info.start = k - m.delay;                         % first burst sample in W
info.error = 'Header';
sym = @(n) z(k + (0:n - 1).' * m.sps);
head = L + 4 * m.headerBytes;
if k + (head - 1) * m.sps > numel(z)
    info.error = 'Incomplete';
    return
end
s = sym(head);
% Carrier offset from the phase advance between the two preamble halves, then phase and gain.
r = s(1:L) .* conj(m.pre);
dphi = angle(sum(r(64:126) .* conj(r(1:63)))) / 63;
rot = @(n) exp(-1j * dphi * (0:n - 1).');
g = mean(r .* rot(L));
info.cfo = dphi / (2 * pi) * m.symbolRate;
s = s .* rot(head) / g;
h = qpsk_demap(s(L + 1:end));
if ~(h(1) == 79 && h(2) == 67 && adler32(h(1:12)) == from_be(h(13:16)))
    return
end
n = from_be(h(5:8));
nd = 4 * n;                                       % data symbols
np = numel(m.pilot) * (ceil(nd / m.segment) + 1);  % pilot symbols
total = head + nd + np;
if n > m.maxPayload || k + (total - 1) * m.sps > numel(z)
    info.error = 'Incomplete';
    return
end
s = sym(total) .* rot(total) / g;
% Phase and gain along the burst from the pilot blocks: known symbols, so no ambiguity and no
% slips at any SNR (blind methods such as the 4th power lock onto interferers when the SNR is
% low). The pilots also take out what frequency offset the preamble estimate left.
[isPilot, blockStart] = pilot_layout(m, nd);
q = s(head + 1:end);
P = numel(m.pilot);
hp = zeros(numel(blockStart), 1);
for b = 1:numel(blockStart)
    hp(b) = mean(q(blockStart(b) + (0:P - 1)) .* conj(m.pilot));
end
centres = blockStart + (P - 1) / 2;
ph = interp1(centres, unwrap(angle(hp)), (1:numel(q)).', 'linear', 'extrap');
amp = interp1(centres, abs(hp), (1:numel(q)).', 'linear', 'extrap');
q = q .* exp(-1j * ph) ./ amp;
p = q(~isPilot);
info.cfo = info.cfo + mean(diff(unwrap(angle(hp))) ./ diff(centres)) / (2 * pi) * m.symbolRate;
bytes = qpsk_demap(p);
ideal = (sign(real(p)) + 1j * sign(imag(p))) / sqrt(2);
info.evm = 20 * log10(sqrt(mean(abs(p - ideal).^2)));
info.symbols = p(1:max(1, floor(numel(p) / 2000)):end);
payload = bitxor(bytes, m.white(1:n));
if adler32(payload) ~= from_be(h(9:12))
    info.error = 'Checksum';
    payload = [];
    return
end
info.error = '';
end

function s = qpsk_map(bytes)
b = bitget(repmat(bytes(:), 1, 8), repmat(8:-1:1, numel(bytes), 1));   % MSB first
b = reshape(b.', 2, []);
s = ((1 - 2 * b(1, :)) + 1j * (1 - 2 * b(2, :))).' / sqrt(2);
end

function bytes = qpsk_demap(s)
b = [real(s(:)) < 0, imag(s(:)) < 0].';
bytes = reshape(b, 8, []).' * (2.^(7:-1:0)).';
end

function v = adler32(d)
d = double(d(:));
n = numel(d);
a = mod(1 + sum(d), 65521);
b = mod(n + sum((n:-1:1).' .* d), 65521);         % exact in double up to megabytes
v = b * 65536 + a;
end

function b = be(v, n)
b = mod(floor(v ./ 256.^(n - 1:-1:0).'), 256);
end

function v = from_be(b)
v = sum(b(:) .* 256.^(numel(b) - 1:-1:0).');
end

% --- frames -----------------------------------------------------------------------------------

function next = video_source(name, scale)
reader = [];
if ~isempty(name)
    try
        reader = VideoReader(name);
    catch err
        fprintf('video_link: cannot read %s (%s); using a test picture\n', name, err.message);
    end
end
base = [];
k = 0;
next = @frame;

    function img = frame()
        k = k + 1;
        if ~isempty(reader)
            if ~hasFrame(reader)
                reader.CurrentTime = 0;
            end
            img = readFrame(reader);
        else
            if isempty(base)
                base = imread('peppers.png');
            end
            img = circshift(base, [0, 8 * k]);
            bar = mod(4 * k, size(img, 1) - 20) + (1:20);
            img(bar, :, :) = 255 - img(bar, :, :);
        end
        img = img(1:scale:end, 1:scale:end, :);
    end
end

function bytes = jpeg_encode(img, quality, file)
imwrite(img, file, 'jpg', 'Quality', quality);
fid = fopen(file, 'r');
bytes = fread(fid, inf, 'uint8=>double');
fclose(fid);
end

function img = jpeg_decode(bytes, file)
fid = fopen(file, 'w');
fwrite(fid, bytes, 'uint8');
fclose(fid);
img = imread(file, 'jpg');
end

function delete_if_exists(file)
if exist(file, 'file')
    delete(file);
end
end

% --- display ----------------------------------------------------------------------------------

function v = make_view()
v.fig = figure('Name', 'Video link', 'NumberTitle', 'off', 'Color', 'w');
v.tx = subplot(2, 2, 1);
v.rx = subplot(2, 2, 2);
v.iq = subplot(2, 2, 3);
v.iqLine = plot(v.iq, NaN, NaN, '.', 'MarkerSize', 4);
axis(v.iq, [-1.5 1.5 -1.5 1.5]);
axis(v.iq, 'square');
grid(v.iq, 'on');
title(v.iq, 'Payload constellation');
v.txt = subplot(2, 2, 4);
axis(v.txt, 'off');
end

function update_view(v, img, last, info, sent, received, failed, throughput, bitRate, bytes)
show_image(v.tx, img,sprintf('Sent frame %d (%.1f kB JPEG)', sent, bytes / 1e3));
if ~isempty(last)
    show_image(v.rx, last, sprintf('Received: %d of %d', received, sent));
end
if ~isempty(info.symbols)
    set(v.iqLine, 'XData', real(info.symbols), 'YData', imag(info.symbols));
end
f = fieldnames(failed);
lines = {sprintf('On air: %.3f Mbit/s QPSK', bitRate / 1e6), ...
         sprintf('JPEG received: %.0f kbit/s', throughput / 1e3), ...
         sprintf('EVM: %.1f dB   CFO: %.0f Hz', info.evm, info.cfo), 'Lost frames:'};
for i = 1:numel(f)
    lines{end + 1} = sprintf('   %s: %d', f{i}, failed.(f{i})); %#ok<AGROW>
end
cla(v.txt);
text(v.txt, 0, 1, lines, 'VerticalAlignment', 'top', 'FontName', 'FixedWidth');
axis(v.txt, 'off');
drawnow limitrate
end

function show_image(ax, img, label)
h = findobj(ax, 'Type', 'image');
if isempty(h) || ~isequal(size(get(h, 'CData')), size(img))
    h = image(ax, img);
    axis(ax, 'image', 'off');
else
    set(h, 'CData', img);
end
title(ax, label);
end
