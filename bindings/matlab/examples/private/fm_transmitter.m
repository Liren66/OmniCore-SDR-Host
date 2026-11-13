function st = fm_transmitter(audio, fa, fs, varargin)
%FM_TRANSMITTER State of a wideband FM modulator; run it with fm_modulate.
%   ST = fm_transmitter(AUDIO, FA, FS) modulates the mono AUDIO (sampled at FA Hz, played in a
%   loop) onto complex baseband at FS Hz with 75 kHz peak deviation and 50 us pre-emphasis.
%   Name-value options: 'Deviation' (Hz, default 75e3), 'Tau' (pre-emphasis, 0 = none),
%   'Offset' (Hz, carrier offset from 0 Hz, default 0), 'Pilot' (true adds the 19 kHz stereo
%   pilot at 10 % deviation, as broadcast stations do), 'Amplitude' (default 0.7).

p = struct('Deviation', 75e3, 'Tau', 50e-6, 'Offset', 0, 'Pilot', false, 'Amplitude', 0.7);
for k = 1:2:numel(varargin)
    p.(varargin{k}) = varargin{k + 1};
end
a = double(audio(:, 1));
a = a - mean(a);
if p.Tau > 0
    % First-order pre-emphasis 1 + s*tau, the inverse of the receiver's de-emphasis.
    r = exp(-1 / (fa * p.Tau));
    a = filter([1, -r] / (1 - r), 1, a);
end
st.audio = a / max(abs(a));
st.fa = fa;
st.fs = fs;
st.p = p;
st.pos = 0;                         % output samples so far
st.phase = 0;
end
