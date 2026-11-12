%TX_TONE_REPEAT Transmit a 1 MHz tone at 2.45 GHz on TX1 in the background for 10 seconds.

dev = omnicore.Device('SampleRate', 30.72e6, 'TxChannels', 1, 'TxFrequency', 2.45e9, 'TxGain', 40);
fs = dev.SampleRate;
x = single(0.7 * exp(1j * 2 * pi * 1e6 / fs * (0:30719).'));  % whole periods: no phase jump
transmitRepeat(dev, x);
pause(10);                                       % MATLAB is free meanwhile
s = stopTransmit(dev);
delete(dev);
fprintf('%d underflows\n', s.Underflow);
