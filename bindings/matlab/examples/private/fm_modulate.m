function [x, st, msg] = fm_modulate(st, n)
%FM_MODULATE The next N samples of a fm_transmitter: X complex baseband at ST.fs.
%   MSG is the modulating signal at ST.fs (audio, interpolated, plus the pilot if enabled).

k = st.pos + (0:n - 1).';
u = k * (st.fa / st.fs);            % position in the audio, in audio samples
na = numel(st.audio);
i0 = floor(u);
fr = u - i0;
a0 = st.audio(mod(i0, na) + 1);
a1 = st.audio(mod(i0 + 1, na) + 1);
msg = a0 + fr .* (a1 - a0);         % linear interpolation; the audio is far below FS / 2
dev = st.p.Deviation;
if st.p.Pilot
    msg = 0.9 * msg + 0.1 * sin(2 * pi * 19e3 / st.fs * k);
end
phase = st.phase + cumsum(2 * pi * (dev * msg + st.p.Offset) / st.fs);
x = st.p.Amplitude * exp(1j * phase);
st.phase = mod(phase(end), 2 * pi);
st.pos = st.pos + n;
end
