function h = fir_lowpass(fc, fs, ntaps)
%FIR_LOWPASS Windowed-sinc low-pass FIR (Hamming window), unity gain at DC. No toolbox needed.
%   H = fir_lowpass(FC, FS, NTAPS): cutoff FC Hz (about the -6 dB point) at sample rate FS. The
%   transition band is roughly 3.3 * FS / NTAPS wide, centred on FC.

n = (0:ntaps - 1).' - (ntaps - 1) / 2;
x = 2 * fc / fs * n;
s = ones(size(x));
nz = x ~= 0;
s(nz) = sin(pi * x(nz)) ./ (pi * x(nz));
w = 0.54 - 0.46 * cos(2 * pi * (0:ntaps - 1).' / (ntaps - 1));
h = s .* w;
h = h / sum(h);
end
