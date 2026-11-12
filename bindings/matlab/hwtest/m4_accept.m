function m4_accept(varargin)
%M4_ACCEPT M4 acceptance runs on a connected OmniCoreSDR (MEX_PLAN.md 8, M4).
%   m4_accept() or m4_accept(Name, Value) with 'Serial' ('' = the only device) and 'Minutes'
%   (full-duplex run, default 10). Needs the staged toolbox folder and bindings/matlab/examples on
%   the path. No RF connection is needed. Errors on the first failure.
%     1. frame-based full duplex (Receiver + Transmitter, 1 ms frames, 30.72 MSPS) for 'Minutes';
%        every 30 s the RX LO, RX gain and TX gain change: no lost frame, overflow or underflow.
%        One TX LO change at the end is reported separately (the core pauses TX for it)
%     2. 8-bit 2T2R at 61.44 MSPS: receive(1e8) as int8 without a gap
%     3. Simulink: examples/simulink_duplex_example runs 2 s without overflow or underflow. The
%        first run after starting MATLAB pauses once for compilation and is reported only

p = inputParser;
p.addParameter('Serial', '');
p.addParameter('Minutes', 10);
p.parse(varargin{:});
o = p.Results;

% 1 -------------------------------------------------------------------------------------------
fs = 30.72e6;
n = 30720;
dev = omnicore.Device(o.Serial, 'SampleRate', fs, 'RxChannels', 1, 'TxChannels', 1, 'RxFrequency', 2.45e9, ...
                      'TxFrequency', 2.45e9, 'RxGainMode', 'manual', 'RxGain', 30, 'TxGain', 10);
fprintf('device %s\n', dev.Serial);
rx = omnicore.Receiver('Device', dev, 'SamplesPerFrame', n);
tx = omnicore.Transmitter('Device', dev);
x = single(0.5 * exp(1j * 2 * pi * 1e6 / fs * (0:n - 1).'));
frames = 0;
invalid = 0;
overflows = 0;
underflows = 0;
retunes = 0;
tic;
next = 30;
while toc < o.Minutes * 60
    [~, valid, overflow] = rx();
    underflow = tx(x);
    frames = frames + 1;
    invalid = invalid + ~valid;
    overflows = overflows + overflow;
    underflows = underflows + underflow;
    if overflow || ~valid || underflow
        fprintf('  frame %d at %.1f s: valid %d, overflow %d, underflow %d\n', frames, toc, valid, overflow, ...
                underflow);
    end
    if toc > next
        next = next + 30;
        retunes = retunes + 1;
        rx.CenterFrequency = 2.45e9 + mod(retunes, 2) * 1e6;
        rx.Gain = 30 - 10 * mod(retunes, 2);
        tx.Gain = 10 + 5 * mod(retunes, 2);
    end
end
check(invalid == 0 && overflows == 0 && underflows == 0, ...
      'full duplex %g min: %d frames, %d retunes; %d invalid, %d overflows, %d underflows', o.Minutes, frames, ...
      retunes, invalid, overflows, underflows);
tx.CenterFrequency = 2.46e9;
bad = 0;
for k = 1:1000
    [~, valid, overflow] = rx();
    bad = bad + ~valid + overflow + tx(x);
end
fprintf('  info  TX LO change while streaming: %d bad frames in the next second\n', bad);
release(rx);
release(tx);
delete(dev);

% 2 -------------------------------------------------------------------------------------------
dev = omnicore.Device(o.Serial, 'IQBits', 8, 'SampleRate', 61.44e6, 'RxBandwidth', 56e6);
[y, meta] = receive(dev, 1e8, 'OutputDataType', 'int8');
check(size(y, 1) == 1e8 && ~meta.Overflow, '8-bit 2T2R %.2f MSPS receive(1e8) int8: %d rows, %d gaps', ...
      dev.SampleRate / 1e6, size(y, 1), size(meta.Gaps, 1));
clear y
delete(dev);

% 3 -------------------------------------------------------------------------------------------
out = simulink_duplex_example(2);
fprintf('  info  Simulink first run: %d frames, %d overflows, underflow at frames %s\n', size(out.y, 3), ...
        nnz(out.overflow), mat2str(find(out.underflow(:)).'));
out = simulink_duplex_example(2);
check(~any(out.overflow) && ~any(out.underflow) && size(out.y, 3) >= 1900, ...
      'Simulink duplex 2 s: %d frames, %d overflows, %d underflows', size(out.y, 3), nnz(out.overflow), ...
      nnz(out.underflow));
fprintf('M4 acceptance passed\n');
end

function check(ok, varargin)
if ok
    fprintf('  ok    %s\n', sprintf(varargin{:}));
else
    error('m4_accept:Failed', 'FAIL  %s', sprintf(varargin{:}));
end
end
