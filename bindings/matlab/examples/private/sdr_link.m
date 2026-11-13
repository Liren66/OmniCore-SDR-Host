function link = sdr_link(cfg)
%SDR_LINK RX1 (and optionally TX1) of an OmniCoreSDR, or a software stand-in for it.
%   LINK = sdr_link(CFG) with CFG a struct; missing fields take these defaults:
%     Simulate     false; true runs without a device (see below)
%     Serial       '' (the only connected device)
%     SampleRate   3.072e6
%     RxFrequency  2.45e9      TxFrequency  same as RxFrequency
%     RxGain       [] = slow AGC, or dB (manual)
%     TxGain       0 dB (89.75 dB attenuation)
%     Bandwidth    analog RX and TX filter, Hz; [] = 0.8 * SampleRate (at least 200 kHz)
%     Tx           false: also open TX1
%     Stream       true: run an RX stream (read, window); false: capture() with receive
%     Pace         false: in simulation, run no faster than real time (for audio and plots)
%     Source       simulation only: @(K, FS, FC) the RF signal at RX sample ticks K (column)
%                  for sample rate FS and RX LO FC, before the RX gain
%     Channel      simulation only: struct with Delay (samples, 3000), Loss (dB from TX to RX
%                  full scale, 30), NoiseDb (dBFS, -75), FrequencyOffset (Hz added to the
%                  TX - RX LO difference, 0)
%
%   LINK is a struct of functions:
%     LINK.start()                start the RX stream (otherwise the first read does). Start it
%                                 before TX in a loop that pairs TX and RX blocks
%     [X, META] = LINK.read(N)    next N RX samples (double column); META.Tick is the hardware
%                                 time of X(1) in samples, META.Lost samples lost before or in X
%     [W, LOST] = LINK.window(T0, N)  the N RX samples from hardware time T0 (ticks), reading the
%                                 stream as far as needed; samples already passed come back as 0
%     X = LINK.capture(N)         one finite capture (Stream false)
%     LINK.write(X, T)            queue X on TX1. T empty: continue one burst (keep it fed);
%                                 T ticks: a separate burst starting at that hardware time
%     T = LINK.now()              hardware time, ticks
%     LINK.configure(Name, Value, ...) / V = LINK.get(Name): SampleRate, RxFrequency,
%                                 TxFrequency, RxGain ([] = AGC), TxGain
%     S = LINK.txStatus()         TX events since the last call (see omnicore.TxStream/status)
%     LINK.close()
%     LINK.device                 the omnicore.Device ([] when simulating)
%
%   Simulation: TX samples come back on RX after Channel.Delay samples, Channel.Loss dB
%   weaker and shifted by the LO difference, plus Source and white noise; the RX gain scales
%   everything relative to 40 dB and the ADC clips at full scale, as the device does.

d = struct('Simulate', false, 'Serial', '', 'SampleRate', 3.072e6, 'RxFrequency', 2.45e9, ...
           'TxFrequency', [], 'RxGain', [], 'TxGain', 0, 'Bandwidth', [], 'Tx', false, 'Stream', true, 'Pace', false, ...
           'Source', [], 'Channel', struct());
for f = fieldnames(d).'
    if ~isfield(cfg, f{1})
        cfg.(f{1}) = d.(f{1});
    end
end
if isempty(cfg.TxFrequency)
    cfg.TxFrequency = cfg.RxFrequency;
end
ch = struct('Delay', 3000, 'Loss', 30, 'NoiseDb', -75, 'FrequencyOffset', 0);
for f = fieldnames(ch).'
    if ~isfield(cfg.Channel, f{1})
        cfg.Channel.(f{1}) = ch.(f{1});
    end
end

dev = [];
rx = [];
tx = [];
carry = zeros(0, 1);                % RX samples read past the last window
carryTick = 0;
rxStarted = false;
% Simulation state
rxTick = 0;
txNext = 0;
segments = cell(0, 2);              % {start tick at RX, samples}
wall = tic;

if isempty(cfg.Bandwidth)
    cfg.Bandwidth = max(200e3, 0.8 * cfg.SampleRate);
end
if ~cfg.Simulate
    args = {'SampleRate', cfg.SampleRate, 'RxChannels', 1, 'RxFrequency', cfg.RxFrequency, ...
            'RxBandwidth', cfg.Bandwidth, 'TxBandwidth', cfg.Bandwidth};
    if isempty(cfg.RxGain)
        args = [args, {'RxGainMode', 'slow'}];
    else
        args = [args, {'RxGainMode', 'manual', 'RxGain', cfg.RxGain}];
    end
    if cfg.Tx
        args = [args, {'TxChannels', 1, 'TxFrequency', cfg.TxFrequency, 'TxGain', cfg.TxGain}];
    else
        args = [args, {'TxChannels', []}];
    end
    if isempty(cfg.Serial)
        dev = omnicore.Device(args{:});
    else
        dev = omnicore.Device(cfg.Serial, args{:});
    end
    cfg.SampleRate = dev.SampleRate;    % as applied by the device
    if cfg.Stream
        rx = rxStream(dev, 'Timeout', 2);   % started by the first read, see readSamples
    end
    if cfg.Tx
        tx = txStream(dev, 'Timeout', 2);
    end
end

link.start = @startRx;
link.read = @readSamples;
link.window = @window;
link.capture = @capture;
link.write = @writeSamples;
link.now = @now;
link.configure = @configureLink;
link.get = @getField;
link.txStatus = @txStatus;
link.close = @closeLink;
link.device = dev;

    function [x, meta] = readSamples(n)
        if ~cfg.Simulate
            startRx();
            [y, m] = read(rx, n);
            x = double(y);
            meta.Tick = double(m.TimeTicks);
            lost = m.Gaps(:, 2);
            lost(isnan(lost)) = 1;
            meta.Lost = sum(lost);
            return
        end
        fs = cfg.SampleRate;
        k = rxTick + (0:n - 1).';
        x = sqrt(10^(cfg.Channel.NoiseDb / 10) / 2) * complex(randn(n, 1), randn(n, 1));
        if ~isempty(cfg.Source)
            x = x + cfg.Source(k, fs, cfg.RxFrequency);
        end
        % TX bursts that overlap these ticks, through the channel.
        g = 10^(-cfg.Channel.Loss / 20);
        fo = cfg.TxFrequency - cfg.RxFrequency + cfg.Channel.FrequencyOffset;
        keep = true(size(segments, 1), 1);
        for s = 1:size(segments, 1)
            first = segments{s, 1};
            data = segments{s, 2};
            lo = max(first, rxTick);
            hi = min(first + numel(data), rxTick + n) - 1;
            if hi >= lo
                kk = (lo:hi).';
                x(kk - rxTick + 1) = x(kk - rxTick + 1) + g * data(kk - first + 1) .* exp(2j * pi * fo / fs * kk);
            end
            keep(s) = first + numel(data) > rxTick + n;
        end
        segments = segments(keep, :);
        if ~isempty(cfg.RxGain)
            x = x * 10^((cfg.RxGain - 40) / 20);
        end
        x = complex(min(max(real(x), -1), 1), min(max(imag(x), -1), 1));   % ADC full scale
        meta.Tick = rxTick;
        meta.Lost = 0;
        rxTick = rxTick + n;
        if cfg.Pace
            ahead = rxTick / fs - toc(wall);
            if ahead > 0
                pause(ahead);
            end
        end
    end

    function startRx()
        % RX starts here (LINK.start or the first read), not when the link opens: anything slow
        % in between (a figure's first render takes ~1 s) would otherwise sit in the RX buffer,
        % and a loop that writes one TX block per RX block would then run that far ahead.
        if ~cfg.Simulate && ~rxStarted
            start(rx);
            rxStarted = true;
        end
    end

    function [w, lost] = window(t0, n)
        w = zeros(n, 1);
        lost = 0;
        while true
            if isempty(carry)
                [carry, m] = readSamples(65536);
                carryTick = m.Tick;
                lost = lost + m.Lost;
            end
            % Copy the overlap of [carryTick, carryTick + numel(carry)) with [t0, t0 + n).
            lo = max(t0, carryTick);
            hi = min(t0 + n, carryTick + numel(carry)) - 1;
            if hi >= lo
                w(lo - t0 + 1:hi - t0 + 1) = carry(lo - carryTick + 1:hi - carryTick + 1);
            end
            if carryTick + numel(carry) >= t0 + n
                used = max(0, t0 + n - carryTick);
                carry = carry(used + 1:end);
                carryTick = carryTick + used;
                return
            end
            carry = zeros(0, 1);
        end
    end

    function x = capture(n)
        if cfg.Simulate
            x = readSamples(n);
        else
            x = double(receive(dev, n, 'Timeout', 2));
        end
    end

    function writeSamples(x, t)
        if ~cfg.Simulate
            if isempty(t)
                write(tx, single(x));
            else
                write(tx, single(x), 'StartTime', t / cfg.SampleRate, 'EndBurst', true);
            end
            return
        end
        if isempty(t)
            t = max(txNext, rxTick);
        end
        segments(end + 1, :) = {t + cfg.Channel.Delay, x(:)};
        txNext = t + numel(x);
    end

    function t = now()
        if cfg.Simulate
            t = rxTick;
        else
            t = double(timeTicks(dev));
        end
    end

    function configureLink(varargin)
        for i = 1:2:numel(varargin)
            cfg.(varargin{i}) = varargin{i + 1};
        end
        if cfg.Simulate
            return
        end
        args = {};
        for i = 1:2:numel(varargin)
            if strcmp(varargin{i}, 'RxGain') && isempty(varargin{i + 1})
                args = [args, {'RxGainMode', 'slow'}]; %#ok<AGROW>
            elseif strcmp(varargin{i}, 'RxGain')
                args = [args, {'RxGainMode', 'manual', 'RxGain', varargin{i + 1}}]; %#ok<AGROW>
            elseif strcmp(varargin{i}, 'Bandwidth')
                args = [args, {'RxBandwidth', varargin{i + 1}, 'TxBandwidth', varargin{i + 1}}]; %#ok<AGROW>
            else
                args = [args, varargin(i:i + 1)]; %#ok<AGROW>
            end
        end
        configure(dev, args{:});
        cfg.SampleRate = dev.SampleRate;
    end

    function v = getField(name)
        v = cfg.(name);
    end

    function s = txStatus()
        if cfg.Simulate || isempty(tx)
            s = struct('Underflow', 0, 'Starved', 0, 'Late', 0, 'BurstAck', 0, 'Errors', 0);
        else
            s = status(tx);
        end
    end

    function closeLink()
        if ~isempty(tx) && isvalid(tx)
            try
                stop(tx);
            catch err
                warning('omnicore:CloseFailed', 'Stopping TX: %s', err.message);
            end
            delete(tx);
        end
        if ~isempty(rx) && isvalid(rx)
            delete(rx);
        end
        if ~isempty(dev) && isvalid(dev)
            delete(dev);
        end
        tx = [];
        rx = [];
        dev = [];
    end
end
