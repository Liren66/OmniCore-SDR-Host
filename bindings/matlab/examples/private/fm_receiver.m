function st = fm_receiver(fs, offset, tau)
%FM_RECEIVER State of a wideband FM (broadcast) demodulator; run it with fm_demodulate.
%   ST = fm_receiver(FS, OFFSET, TAU) for complex baseband at FS Hz where the station sits OFFSET
%   Hz from the centre (tune the LO OFFSET below the station to keep it off the LO leakage and
%   DC). TAU is the de-emphasis time constant: 50e-6 (China, Europe) or 75e-6 (Americas, Korea).
%
%   Chain: mix the station to 0 Hz -> channel filter (+-110 kHz) and decimate to about 384 kHz ->
%   phase discriminator (75 kHz deviation = 1) -> de-emphasis -> 15 kHz audio filter and
%   decimate by 8 to about 48 kHz.

d1 = max(1, round(fs / 384e3));
st.fs = fs;
st.fsMpx = fs / d1;                 % composite (MPX) rate after the channel filter
st.fsAudio = st.fsMpx / 8;
st.offset = offset;
st.mix = 0;                         % mixer phase at the next input sample
st.channel = fir_decimator(fir_lowpass(110e3, fs, 16 * d1), d1);
st.audio = fir_decimator(fir_lowpass(16e3, st.fsMpx, 160), 8);
st.last = complex(1);               % previous channel sample for the discriminator
st.gain = st.fsMpx / (2 * pi * 75e3);
if tau > 0
    st.deemphA = exp(-1 / (st.fsMpx * tau));
else
    st.deemphA = 0;
end
st.deemphZ = 0;
end
