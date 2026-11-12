function m5_accept(varargin)
%M5_ACCEPT M5 acceptance runs on a connected OmniCoreSDR (MEX_PLAN.md 8, M5).
%   m5_accept() uses the board's internal PPS. m5_accept('External', true) needs a 10 MHz
%   reference and a PPS on the board's inputs and uses them instead. 'Serial' selects a device.
%   Needs the staged toolbox folder on the path. Errors on the first failure.
%     1. PPS-aligned time: setTime(0, 'AtNextPPS', true); afterwards PPS edges fall on whole
%        seconds within +-1 sample (the PPS is asynchronous to the sample clock) and a capture at a
%        whole-second StartTime has meta.Time on that second
%     2. (External) RefLocked / PpsLocked in status
%   Ctrl+C (needs a person at the keyboard): see the message printed at the end.

p = inputParser;
p.addParameter('Serial', '');
p.addParameter('External', false);
p.parse(varargin{:});
o = p.Results;

dev = omnicore.Device(o.Serial);
cleanup = onCleanup(@() restore(dev));
fs = dev.SampleRate;
if o.External
    dev.ClockSource = 'external';
    dev.PpsEnable = true;
    dev.TimeSource = 'external';
    pause(3);
    s = status(dev);
    check(s.RefLocked && s.PpsLocked, 'external reference: RefLocked %d, PpsLocked %d', s.RefLocked, s.PpsLocked);
else
    dev.TimeSource = 'internal';
end
fprintf('device %s, time source %s, clock %s\n', dev.Serial, dev.TimeSource, dev.ClockSource);
pause(1.2);
setTime(dev, 0, 'AtNextPPS', true);
pause(1.2);
[~, first] = lastPPS(dev);
fprintf('  info  first edge after the load: %d ticks (%d = one second)\n', first, fs);
edges = zeros(1, 5, 'uint64');
for k = 1:5
    pause(1);
    [~, edges(k)] = lastPPS(dev);
end
% The PPS edge is not synchronous to the sample clock: it latches with +-1 sample.
off = double(mod(edges + 1, uint64(fs))) - 1;
check(all(abs(off) <= 1), 'PPS edges at %s ticks: whole seconds (offsets %s samples)', mat2str(double(edges)), ...
      mat2str(off));
start = ceil(time(dev)) + 1;
[~, meta] = receive(dev, 1e5, 'StartTime', start, 'Timeout', 2);
check(meta.Time == start && meta.TimeTicks == uint64(start * fs), 'capture at StartTime %d s: meta.Time %.9f', ...
      start, meta.Time);
fprintf('M5 acceptance passed\n');
fprintf(['\nCtrl+C (by hand, MATLAB desktop): run\n' ...
         '    dev = omnicore.Device(); for k = 1:100, receive(dev, 3e8, ''OutputDataType'', ''int16''); end\n' ...
         'press Ctrl+C, then delete(dev) and check `och info` opens the device. With a MEX built with\n' ...
         'OCH_MATLAB_CTRL_C=ON the receive itself stops within 0.1 s (omnicore:Interrupted); without it\n' ...
         'MATLAB stops after the current receive returns (up to 10 s at 30.72 MSPS).\n']);
end

function restore(dev)
if isvalid(dev) && dev.isOpen()
    dev.TimeSource = 'none';
    dev.ClockSource = 'internal';
    delete(dev);
end
end

function check(ok, varargin)
if ok
    fprintf('  ok    %s\n', sprintf(varargin{:}));
else
    error('m5_accept:Failed', 'FAIL  %s', sprintf(varargin{:}));
end
end
