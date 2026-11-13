function [audio, st, mpx] = fm_demodulate(x, st)
%FM_DEMODULATE Demodulate the next block X (complex column at ST.fs) with a fm_receiver state.
%   AUDIO is mono audio at ST.fsAudio (about 48 kHz, +-1 at full deviation). MPX is the
%   discriminator output at ST.fsMpx before de-emphasis: the composite signal with mono audio
%   up to 15 kHz, the 19 kHz stereo pilot, L-R around 38 kHz and RDS at 57 kHz.

x = double(x(:));
if st.offset ~= 0
    w = 2 * pi * st.offset / st.fs;
    x = x .* exp(-1j * (st.mix + w * (0:numel(x) - 1).'));
    st.mix = mod(st.mix + w * numel(x), 2 * pi);
end
[z, st.channel] = fir_decimate(x, st.channel);
if isempty(z)
    audio = zeros(0, 1);
    mpx = zeros(0, 1);
    return
end
mpx = angle(z .* conj([st.last; z(1:end - 1)])) * st.gain;
st.last = z(end);
a = st.deemphA;
[y, st.deemphZ] = filter(1 - a, [1, -a], mpx, st.deemphZ);
[audio, st.audio] = fir_decimate(y, st.audio);
end
