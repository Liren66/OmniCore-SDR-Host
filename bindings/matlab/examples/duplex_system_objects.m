%DUPLEX_SYSTEM_OBJECTS Frame-based full duplex with omnicore.Receiver and omnicore.Transmitter.
%   Transmits a 1 MHz tone on TX1 while receiving RX1, one frame each per loop, for 10 seconds,
%   and retunes the receiver halfway. The loop runs at the device rate because rx() waits for
%   each frame. Connect TX1 to RX1 through >= 30 dB attenuation to see the tone, or use antennas.

fs = 30.72e6;
n = 30720;                                        % 1 ms frames
dev = omnicore.Device('SampleRate', fs, 'RxChannels', 1, 'TxChannels', 1, ...
                      'RxFrequency', 2.45e9, 'TxFrequency', 2.45e9, 'RxGainMode', 'manual', 'RxGain', 20);
rx = omnicore.Receiver('Device', dev, 'SamplesPerFrame', n);
tx = omnicore.Transmitter('Device', dev, 'Gain', 30);
x = single(0.5 * exp(1j * 2 * pi * 1e6 / fs * (0:n - 1).'));

frames = 10 * fs / n;
power = zeros(frames, 1);
lost = 0;
underflows = 0;
for k = 1:frames
    [y, valid, overflow] = rx();
    underflows = underflows + tx(x);
    lost = lost + overflow + ~valid;
    power(k) = mean(abs(double(y)).^2);
    if k == frames / 2
        rx.CenterFrequency = 2.451e9;             % tunable: the stream keeps running
    end
end
release(rx);
release(tx);
delete(dev);
fprintf('%d frames: %d lost or late, %d with TX underflow, mean RX power %.1f dBFS\n', frames, lost, ...
        underflows, 10 * log10(mean(power)));
