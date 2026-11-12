function offsets = timed_burst_loopback(bursts)
%TIMED_BURST_LOOPBACK Timed TX bursts against a running RX stream: the TX -> RX delay in samples.
%   OFFSETS = timed_burst_loopback(BURSTS) (default 10). Connect TX1 to RX1 through >= 30 dB
%   attenuation. Each 20000-sample tone burst is scheduled 0.3 s ahead on the hardware clock and
%   found in the RX stream by its sample time: OFFSETS(k) is how many samples after its StartTime
%   the burst's first sample appears (the TX + RX pipeline delay). It should be the same for every
%   burst. Without the cable RX sees noise only and the offsets mean nothing.
%
%   RX and TX each run as one stream for the whole measurement: while one direction streams, the
%   other cannot be stopped and started again (the core resets the USB pipes only when both are
%   stopped), so a receive() per burst would fail with omnicore:BUSY.

if nargin < 1
    bursts = 10;
end
fs = 30.72e6;
n = 20000;
lead = 1000;                                      % samples kept before each StartTime
dev = omnicore.Device('SampleRate', fs, 'RxChannels', 1, 'TxChannels', 1, 'RxFrequency', 2.45e9, ...
                      'TxFrequency', 2.45e9, 'TxGain', 40, 'RxGainMode', 'manual', 'RxGain', 20);
cleanup = onCleanup(@() delete(dev));
x = single(0.5 * exp(1j * 2 * pi * 1e6 / fs * (0:n - 1).'));
rx = rxStream(dev);
tx = txStream(dev);
start(rx);
offsets = zeros(1, bursts);
for k = 1:bursts
    t = round((time(dev) + 0.3) * fs);            % StartTime in ticks
    write(tx, x, 'StartTime', t / fs, 'EndBurst', true);
    % Read until the window [t - lead, t + 3n) has passed, keeping only its samples.
    window = complex(zeros(lead + 3 * n, 1, 'single'));
    while true
        [y, meta] = read(rx, 65536);
        first = double(meta.TimeTicks);
        rows = (1:size(y, 1)).' + first - (t - lead);     % positions in the window
        keep = rows >= 1 & rows <= numel(window);
        window(rows(keep)) = y(keep);
        if first + size(y, 1) >= t + 3 * n
            break
        end
    end
    p = abs(double(window)).^2;
    onset = find(p > 0.5 * max(p), 1);            % first sample above half the peak power
    offsets(k) = onset - 1 - lead;
end
stop(rx);
s = status(tx);
stop(tx);
fprintf('burst onset after StartTime: %s samples; %d bursts acknowledged, %d late\n', mat2str(offsets), ...
        s.BurstAck, s.Late);
end
