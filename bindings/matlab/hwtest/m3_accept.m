function m3_accept(varargin)
%M3_ACCEPT M3 acceptance runs on a connected OmniCoreSDR (MEX_PLAN.md 8, M3).
%   m3_accept() runs the checks that need no RF connection; m3_accept(Name, Value) with
%     'Serial'    device serial ('' = the only one)
%     'Minutes'   length of the repeat run, default 10
%     'Loopback'  true when TX1 is cabled to RX1 through >= 30 dB attenuation: also checks
%                 the timed burst loopback, default false
%   Needs the staged toolbox folder on the path. Prints one line per check and errors on the
%   first failure.
%     1. transmitRepeat of a tone for 'Minutes': no underflow. 2R RX reads alongside; its
%        overflows are listed for information (full duplex for 10 minutes is the M4 check)
%     2. 20 timed bursts 0.1 s apart: every acknowledgement at start + N (hardware time)
%     3. (Loopback) timed TX burst + timed RX: the burst appears at the same sample every time

p = inputParser;
p.addParameter('Serial', '');
p.addParameter('Minutes', 10);
p.addParameter('Loopback', false);
p.parse(varargin{:});
o = p.Results;

dev = omnicore.Device(o.Serial, 'SampleRate', 30.72e6, 'RxChannels', [1 2], 'TxChannels', 1, ...
                      'TxFrequency', 2.45e9, 'RxFrequency', 2.45e9, 'TxGain', 20);
cleanup = onCleanup(@() delete(dev));
fs = dev.SampleRate;
fprintf('device %s, %.6g MSPS, RX [%s], TX [%s]\n', dev.Serial, fs / 1e6, num2str(dev.RxChannels), ...
        num2str(dev.TxChannels));
tone = single(0.5 * exp(1j * 2 * pi * 1e6 / fs * (0:30719).')); % 1 MHz, whole periods

% 1 -------------------------------------------------------------------------------------------
transmitRepeat(dev, tone);
rx = rxStream(dev);
start(rx);
underflow = 0;
starved = 0;
overflows = 0;
reads = 0;
tic;
next = 10;
while toc < o.Minutes * 60
    [~, m] = read(rx, 65536);
    reads = reads + 1;
    if m.Overflow
        overflows = overflows + 1;
        fprintf('  RX overflow at %.1f s: %g samples lost\n', toc, sum(m.Gaps(:, 2)));
    end
    if toc > next
        next = next + 10;
        s = status(dev);
        if s.Temperature > 0
            fprintf('  %4.0f s  %.1f degC\n', toc, s.Temperature);
        end
    end
end
stop(rx);
s = stopTransmit(dev);
underflow = underflow + s.Underflow;
starved = starved + s.Starved;
check(underflow == 0 && starved == 0 && s.Errors == 0, ...
      'repeat %g min: %d underflows, %d starved, %d errors; RX alongside %d reads, %d overflows', ...
      o.Minutes, underflow, starved, s.Errors, reads, overflows);
fprintf('  info  RX alongside the repeat: %d of %d reads had an overflow\n', overflows, reads);

% 2 -------------------------------------------------------------------------------------------
tx = txStream(dev);
n = 20000;
t0 = time(dev) + 0.3;
starts = round((t0 + 0.1 * (0:19)) * fs);
for k = 1:20
    write(tx, tone(1:n), 'StartTime', starts(k) / fs, 'EndBurst', true);
end
pause(2.5);
s = status(tx);
stop(tx);
delta = double(s.AckTicks(:).') - starts(1:numel(s.AckTicks));
check(s.BurstAck == 20 && s.Late == 0 && s.Underflow == 0, 'timed bursts: %d acks, %d late, %d underflows', ...
      s.BurstAck, s.Late, s.Underflow);
check(all(delta == delta(1)), 'timed bursts: ack - start = %s samples (N = %d)', ...
      mat2str(unique(delta)), n);

% 3 -------------------------------------------------------------------------------------------
if o.Loopback
    configure(dev, 'RxGainMode', 'manual', 'RxGain', 20);
    offsets = zeros(1, 10);
    for k = 1:10
        t = time(dev) + 0.3;
        tx = txStream(dev);
        write(tx, tone(1:n), 'StartTime', t, 'EndBurst', true);
        [x, meta] = receive(dev, 3 * n, 'StartTime', t - n / fs, 'Timeout', 2);
        stop(tx);
        delete(tx);
        p = abs(x(:, 1)).^2;
        onset = find(p > 0.5 * max(p), 1);
        offsets(k) = double(meta.TimeTicks) + onset - 1 - round(t * fs);
    end
    check(max(offsets) - min(offsets) <= 1, 'loopback: burst at %s samples after StartTime', mat2str(offsets));
end
fprintf('M3 acceptance passed\n');
end

function check(ok, varargin)
if ok
    fprintf('  ok    %s\n', sprintf(varargin{:}));
else
    error('m3_accept:Failed', 'FAIL  %s', sprintf(varargin{:}));
end
end
