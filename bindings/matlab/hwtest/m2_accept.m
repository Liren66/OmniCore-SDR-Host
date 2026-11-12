function m2_accept(serial)
%M2_ACCEPT M2 acceptance runs on a connected OmniCoreSDR (MEX_PLAN.md 8, M2).
%   m2_accept() or m2_accept(SERIAL). Needs the staged toolbox folder on the path. No RF
%   connection is needed. Prints one line per check and errors on the first failure.
%     1. receive 1e8 samples, 2R at 30.72 MSPS (int16, 800 MB): no gap
%     2. RxStream read(65536) loop for 60 s without processing: no overflow
%     3. pause(1) between reads: meta.Gaps reports the loss, matching the time jump

if nargin < 1
    serial = '';
end
dev = omnicore.Device(serial, 'SampleRate', 30.72e6, 'RxChannels', [1 2]);
cleanup = onCleanup(@() delete(dev));
fprintf('device %s, %.6g MSPS, RX [%s]\n', dev.Serial, dev.SampleRate / 1e6, num2str(dev.RxChannels));

% 1 -------------------------------------------------------------------------------------------
n = 1e8;
tic;
[x, meta] = receive(dev, n, 'OutputDataType', 'int16');
t = toc;
check(size(x, 1) == n && ~meta.Overflow, 'receive 1e8: %d rows, %d gaps, %.2f s (%.2f s of signal)', ...
      size(x, 1), size(meta.Gaps, 1), t, n / dev.SampleRate);
clear x

% 2 -------------------------------------------------------------------------------------------
rx = rxStream(dev);
start(rx);
reads = 0;
overflows = 0;
lost = 0;
tic;
while toc < 60
    [~, m] = read(rx, 65536);
    reads = reads + 1;
    if m.Overflow
        overflows = overflows + 1;
        lost = lost + sum(m.Gaps(:, 2));
    end
end
stop(rx);
check(overflows == 0, 'stream 60 s: %d reads (%.3g samples), %d overflows, %g lost', reads, reads * 65536, ...
      overflows, lost);

% 3 -------------------------------------------------------------------------------------------
rx = rxStream(dev, 'BufferBytes', 16 * 2^20);
start(rx);
[~, before] = read(rx, 65536);
pause(1);
reads = 0;
m = before;
while true
    prev = m;
    [~, m] = read(rx, 65536);
    reads = reads + 1;
    if m.Overflow || reads > 200
        break
    end
end
stop(rx);
check(m.Overflow, 'pause(1): overflow reported after %d reads', reads);
if m.Gaps(1, 1) == 1
    jump = double(m.TimeTicks - (prev.TimeTicks + 65536));
else
    jump = m.Gaps(1, 2); % gap inside the read: the start is contiguous
    check(m.TimeTicks == prev.TimeTicks + 65536, 'pause(1): read starts where the previous ended');
end
check(jump == m.Gaps(1, 2), 'pause(1): Gaps = [%g %g], %.3f s lost, time jump %g', m.Gaps(1, 1), ...
      m.Gaps(1, 2), m.Gaps(1, 2) / dev.SampleRate, jump);
fprintf('M2 acceptance passed\n');
end

function check(ok, varargin)
if ok
    fprintf('  ok    %s\n', sprintf(varargin{:}));
else
    error('m2_accept:Failed', 'FAIL  %s', sprintf(varargin{:}));
end
end
