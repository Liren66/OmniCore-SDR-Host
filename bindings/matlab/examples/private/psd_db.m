function [p, f] = psd_db(x, fs, nfft, onesided)
%PSD_DB Averaged power spectrum in dB (dBFS for samples scaled to +-1). No toolbox needed.
%   [P, F] = psd_db(X, FS, NFFT) splits X into NFFT-sample Hann-windowed frames and averages
%   their periodograms: P centred on 0 Hz, F in Hz from -FS/2. psd_db(..., true) returns the
%   one-sided spectrum 0..FS/2 of a real X.

if nargin < 4
    onesided = false;
end
x = double(x(:));
frames = floor(numel(x) / nfft);
if frames < 1
    x(end + 1:nfft) = 0;
    frames = 1;
end
w = 0.5 - 0.5 * cos(2 * pi * (0:nfft - 1).' / nfft);
X = fft(reshape(x(1:frames * nfft), nfft, frames) .* w);
p = mean(abs(X).^2, 2) / sum(w)^2;      % a full-scale tone reads 0 dB
if onesided
    p = p(1:nfft / 2 + 1);
    p(2:end - 1) = 2 * p(2:end - 1);
    f = (0:nfft / 2).' * fs / nfft;
else
    p = fftshift(p);
    f = (-nfft / 2:nfft / 2 - 1).' * fs / nfft;
end
p = 10 * log10(p + 1e-20);
end
