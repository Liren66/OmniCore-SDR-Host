function sdr_scope(varargin)
%SDR_SCOPE A simple oscilloscope and spectrum analyser on an OmniCoreSDR receiver.
%   sdr_scope() opens a window with the RX1 waveform (with an edge trigger) and its spectrum,
%   and controls on the right: centre frequency, sample rate, gain or AGC, RX channel, what to
%   draw (I and Q, magnitude or phase), trigger mode and level, time span, spectrum averaging
%   and peak hold, Run / Stop and Single. Close the window to stop.
%
%   sdr_scope(Name, Value) with
%     'Frequency'   centre frequency, Hz, default 2.45e9
%     'SampleRate'  Hz, default 30.72e6
%     'Seconds'     close by itself after this long, default Inf
%     'Serial'      device serial number, '' = the only connected device
%     'Simulate'    true: no device; a test signal with a 1.2 MHz tone, a pulsed chirp every
%                   200 us at -3 MHz and noise, at fixed RF frequencies (retuning moves them)
%
%   Each refresh is one finite capture (omnicore.Device/receive) of twice the time span, or of
%   the spectrum frames if more: a scope needs fresh snapshots, not every sample. The trigger
%   searches the first half for the edge (rising or falling through Level on the trace drawn:
%   I, or the magnitude) and draws the span from there; without an edge it shows the untriggered
%   capture ("auto" mode) and says so.
%
%   Example:
%       sdr_scope('Frequency', 433.92e6, 'SampleRate', 5e6)
%
%   See also omnicore.Device/receive, rx_spectrum.

opt = struct('Frequency', 2.45e9, 'SampleRate', 30.72e6, 'Seconds', Inf, 'Serial', '', 'Simulate', false);
opt = parse_options(opt, varargin);
rates = [2.5e6, 5e6, 10e6, 15.36e6, 30.72e6, 61.44e6];
[~, rateIndex] = min(abs(rates - opt.SampleRate));

link = sdr_link(struct('Simulate', opt.Simulate, 'Serial', opt.Serial, 'SampleRate', rates(rateIndex), ...
                       'RxFrequency', opt.Frequency, 'RxGain', 30, 'Stream', false, ...
                       'Source', @test_signal));
cleanup = onCleanup(@() link.close());

s = struct('running', true, 'single', false, 'agc', false, 'gain', 30, 'mode', 1, 'trigger', 2, ...
           'level', 0.1, 'spanUs', 20, 'average', 8, 'hold', false, 'holdP', [], 'busy', false);
ui = build_ui();
clock = tic;
while ishghandle(ui.fig) && toc(clock) < opt.Seconds
    if s.running || s.single
        try
            refresh();
        catch err
            set(ui.status, 'String', err.message, 'ForegroundColor', [0.8 0 0]);
            s.running = false;
            set(ui.run, 'Value', 0, 'String', 'Run');
        end
        s.single = false;
        drawnow limitrate
        pause(0.02);
    else
        pause(0.05);
    end
end
if ishghandle(ui.fig)
    close(ui.fig);
end

    function refresh()
        fs = link.get('SampleRate');
        span = min(max(round(s.spanUs * 1e-6 * fs), 64), 500000);
        nfft = 4096;
        n = max(2 * span, nfft * s.average);
        x = link.capture(n);
        % Trace to draw (and trigger on).
        switch s.mode
            case 1
                trace = real(x);
            case 2
                trace = abs(x);
            otherwise
                trace = angle(x) / pi;
        end
        first = 1;
        status = 'Free run';
        if s.trigger > 1
            a = trace(1:n - span);
            if s.trigger == 2
                edges = find(a(1:end - 1) < s.level & a(2:end) >= s.level, 1);
            else
                edges = find(a(1:end - 1) > s.level & a(2:end) <= s.level, 1);
            end
            if isempty(edges)
                status = 'Not triggered (auto)';
            else
                first = edges + 1;
                status = 'Triggered';
            end
        end
        k = first:first + span - 1;
        t = (0:span - 1).' / fs * 1e6;
        set(ui.t1, 'XData', t, 'YData', trace(k));
        if s.mode == 1
            set(ui.t2, 'XData', t, 'YData', imag(x(k)), 'Visible', 'on');
        else
            set(ui.t2, 'Visible', 'off');
        end
        set(ui.tl, 'YData', [s.level, s.level], 'XData', [0, t(end)], ...
            'Visible', onoff(s.trigger > 1));
        xlim(ui.time, [0, max(t(end), eps)]);
        [p, f] = psd_db(x(1:nfft * s.average), fs, nfft);
        if s.hold
            if isempty(s.holdP) || numel(s.holdP) ~= numel(p)
                s.holdP = p;
            end
            s.holdP = max(s.holdP, p);
            set(ui.sh, 'XData', (link.get('RxFrequency') + f) / 1e6, 'YData', s.holdP, 'Visible', 'on');
        else
            set(ui.sh, 'Visible', 'off');
        end
        fc = link.get('RxFrequency');
        set(ui.sl, 'XData', (fc + f) / 1e6, 'YData', p);
        xlim(ui.spec, (fc + [-fs, fs] / 2) / 1e6);
        [pk, ip] = max(p);
        power = 10 * log10(mean(abs(x).^2) + 1e-20);
        set(ui.meas, 'String', sprintf(['Power %.1f dBFS   peak %.1f dBFS at %.4f MHz   ' ...
                                        'I p-p %.3f   clipped %d'], power, pk, (fc + f(ip)) / 1e6, ...
                                       max(real(x(k))) - min(real(x(k))), ...
                                       sum(abs(real(x)) >= 0.999 | abs(imag(x)) >= 0.999)));
        set(ui.status, 'String', status, 'ForegroundColor', [0 0 0]);
        title(ui.time, sprintf('%s, %.2f MSPS, %d samples', ui.modeNames{s.mode}, fs / 1e6, span));
    end

    function ui = build_ui()
        ui.fig = figure('Name', 'OmniCoreSDR scope', 'NumberTitle', 'off', 'Color', 'w', ...
                        'Position', [100, 100, 1100, 650]);
        ui.time = axes(ui.fig, 'Position', [0.06, 0.58, 0.62, 0.36]);
        hold(ui.time, 'on');
        ui.t1 = plot(ui.time, NaN, NaN);
        ui.t2 = plot(ui.time, NaN, NaN);
        ui.tl = plot(ui.time, NaN, NaN, ':', 'Color', [0.5 0.5 0.5]);
        grid(ui.time, 'on');
        xlabel(ui.time, '\mus');
        ylim(ui.time, [-1.05, 1.05]);
        ui.spec = axes(ui.fig, 'Position', [0.06, 0.1, 0.62, 0.36]);
        hold(ui.spec, 'on');
        ui.sh = plot(ui.spec, NaN, NaN, 'Color', [0.85 0.6 0.6]);
        ui.sl = plot(ui.spec, NaN, NaN);
        grid(ui.spec, 'on');
        xlabel(ui.spec, 'MHz');
        ylabel(ui.spec, 'dBFS');
        ylim(ui.spec, [-130, 0]);
        ui.meas = uicontrol(ui.fig, 'Style', 'text', 'Units', 'normalized', ...
                            'Position', [0.06, 0.005, 0.62, 0.035], 'BackgroundColor', 'w', ...
                            'HorizontalAlignment', 'left');
        ui.modeNames = {'I (blue) and Q', 'Magnitude', 'Phase / pi'};
        p = uipanel(ui.fig, 'Position', [0.71, 0.02, 0.27, 0.96], 'Title', 'Controls', ...
                    'BackgroundColor', 'w');
        row = 0;
        ui.freq = field('Centre MHz', 'edit', sprintf('%.6g', link.get('RxFrequency') / 1e6), @onFrequency);
        ui.rate = field('Sample rate', 'popupmenu', arrayfun(@(r) sprintf('%.5g MSPS', r / 1e6), rates, ...
                        'UniformOutput', false), @onRate);
        set(ui.rate, 'Value', rateIndex);
        ui.gain = field('RX gain dB', 'edit', num2str(s.gain), @onGain);
        ui.agc = field('AGC', 'checkbox', '', @onAgc);
        ui.chan = field('Channel', 'popupmenu', {'RX1', 'RX2'}, @onChannel);
        ui.mode = field('Display', 'popupmenu', ui.modeNames, @(h, ~) setfield_('mode', get(h, 'Value')));
        ui.trig = field('Trigger', 'popupmenu', {'Free run', 'Rising edge', 'Falling edge'}, ...
                        @(h, ~) setfield_('trigger', get(h, 'Value')));
        set(ui.trig, 'Value', s.trigger);
        ui.level = field('Level', 'edit', num2str(s.level), @(h, ~) setnum('level', h, -1, 1));
        ui.span = field('Span us', 'edit', num2str(s.spanUs), @(h, ~) setnum('spanUs', h, 1, 20000));
        ui.avg = field('Averages', 'edit', num2str(s.average), @(h, ~) setnum('average', h, 1, 64));
        ui.hold = field('Peak hold', 'checkbox', '', @onHold);
        row = row + 1.2;
        ui.run = uicontrol(p, 'Style', 'togglebutton', 'String', 'Stop', 'Value', 1, 'Units', 'normalized', ...
                           'Position', [0.05, 0.95 - 0.065 * row, 0.42, 0.055], 'Callback', @onRun);
        uicontrol(p, 'Style', 'pushbutton', 'String', 'Single', 'Units', 'normalized', ...
                  'Position', [0.53, 0.95 - 0.065 * row, 0.42, 0.055], 'Callback', @onSingle);
        row = row + 1;
        ui.status = uicontrol(p, 'Style', 'text', 'String', '', 'Units', 'normalized', ...
                              'Position', [0.05, max(0.01, 0.95 - 0.065 * row - 0.05), 0.9, 0.08], ...
                              'BackgroundColor', 'w', 'HorizontalAlignment', 'left');
        if opt.Simulate
            set(ui.fig, 'Name', 'OmniCoreSDR scope (simulated)');
        end

        function h = field(label, style, value, callback)
            row = row + 1;
            y = 0.95 - 0.065 * row;
            uicontrol(p, 'Style', 'text', 'String', label, 'Units', 'normalized', ...
                      'Position', [0.05, y, 0.42, 0.045], 'BackgroundColor', 'w', ...
                      'HorizontalAlignment', 'left');
            h = uicontrol(p, 'Style', style, 'String', value, 'Units', 'normalized', ...
                          'Position', [0.5, y, 0.45, 0.05], 'Callback', callback);
        end
    end

    % --- control callbacks (they run during drawnow/pause, between captures) ------------------
    function onFrequency(h, ~)
        v = str2double(get(h, 'String')) * 1e6;
        apply(@() link.configure('RxFrequency', v));
        s.holdP = [];
    end

    function onRate(h, ~)
        r = rates(get(h, 'Value'));
        apply(@() link.configure('SampleRate', r, 'Bandwidth', max(200e3, min(56e6, 0.8 * r))));
        s.holdP = [];
    end

    function onGain(h, ~)
        s.gain = str2double(get(h, 'String'));
        if ~s.agc
            apply(@() link.configure('RxGain', s.gain));
        end
    end

    function onAgc(h, ~)
        s.agc = logical(get(h, 'Value'));
        if s.agc
            apply(@() link.configure('RxGain', []));
            set(ui.gain, 'Enable', 'off');
        else
            apply(@() link.configure('RxGain', s.gain));
            set(ui.gain, 'Enable', 'on');
        end
    end

    function onChannel(h, ~)
        if ~opt.Simulate
            apply(@() link.configure('RxChannels', get(h, 'Value')));
        end
    end

    function onHold(h, ~)
        s.hold = logical(get(h, 'Value'));
        s.holdP = [];
    end

    function onRun(h, ~)
        s.running = logical(get(h, 'Value'));
        set(h, 'String', ifelse(s.running, 'Stop', 'Run'));
    end

    function onSingle(~, ~)
        s.single = true;
        s.running = false;
        set(ui.run, 'Value', 0, 'String', 'Run');
    end

    function setfield_(name, v)
        s.(name) = v;
    end

    function setnum(name, h, lo, hi)
        v = str2double(get(h, 'String'));
        if isnan(v)
            v = s.(name);
        end
        s.(name) = min(max(v, lo), hi);
        set(h, 'String', num2str(s.(name)));
    end

    function apply(f)
        try
            f();
            set(ui.status, 'String', '', 'ForegroundColor', [0 0 0]);
        catch err
            set(ui.status, 'String', err.message, 'ForegroundColor', [0.8 0 0]);
        end
    end

    % --- simulated input ------------------------------------------------------------------
    function x = test_signal(k, fs, fc)
        t = k / fs;
        x = zeros(size(k));
        f1 = 2.4512e9 - fc;                       % a tone, 1.2 MHz above the default centre
        if abs(f1) < fs / 2
            x = x + 0.4 * exp(2j * pi * f1 * t);
        end
        f2 = 2.447e9 - fc;                        % a 20 us chirp every 200 us, 3 MHz below
        if abs(f2) < fs / 2
            tp = mod(t, 200e-6);
            on = tp < 20e-6;
            x(on) = x(on) + 0.6 * exp(2j * pi * (f2 * t(on) + 0.5 * 2e6 / 20e-6 * tp(on).^2));
        end
    end
end

function v = onoff(tf)
v = 'off';
if tf
    v = 'on';
end
end

function v = ifelse(c, a, b)
if c
    v = a;
else
    v = b;
end
end
