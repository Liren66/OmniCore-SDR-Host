%RX_SPECTRUM Capture 1e6 samples at 2.45 GHz and plot the spectrum of RX1.

dev = omnicore.Device('SampleRate', 30.72e6, 'RxFrequency', 2.45e9, 'RxBandwidth', 20e6, 'RxChannels', 1);
[x, meta] = receive(dev, 1e6);
fs = dev.SampleRate;
fc = dev.RxFrequency;
delete(dev);
if meta.Overflow
    warning('%g samples were lost', sum(meta.Gaps(:, 2)));
end
n = 4096;
w = 0.5 - 0.5 * cos(2 * pi * (0:n - 1).' / n);   % Hann, no toolbox needed
frames = floor(numel(x) / n);
p = zeros(n, 1);
for k = 1:frames
    p = p + abs(fftshift(fft(double(x((k - 1) * n + (1:n))) .* w))).^2;
end
p = p / frames / sum(w.^2);
f = (fc + (-n / 2:n / 2 - 1).' * fs / n) / 1e6;
plot(f, 10 * log10(p));
xlabel('MHz');
ylabel('dBFS');
title(sprintf('RX1 at %.3f MHz, %.2f MSPS', fc / 1e6, fs / 1e6));
grid on
