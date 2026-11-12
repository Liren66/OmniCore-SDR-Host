classdef (TestTags = {'Hardware'}) TxHardwareTest < matlab.unittest.TestCase
    %TXHARDWARETEST M3 checks on a connected OmniCoreSDR: transmit, transmitRepeat, TxStream, timed
    %   RX and TX (MEX_PLAN.md 8). No RF connection is needed (the TX output is not checked; use a
    %   50 ohm load or an antenna, at the default low TxGain). Skipped when no device is connected.
    %       results = runtests('TxHardwareTest')
    %   The 10-minute repeat run is in hwtest/m3_accept.m.

    properties
        Serial = ''
    end

    methods (TestClassSetup)
        function findDevice(tc)
            tc.assumeEqual(omnicore_mex('count'), 0, 'close every omnicore.Device before running the tests');
            devices = omnicore.findDevices();
            tc.assumeNotEmpty(devices, 'no OmniCoreSDR connected');
            tc.Serial = getenv('OMNICORE_TEST_SERIAL');
            if isempty(tc.Serial)
                tc.Serial = devices(1).Serial;
            end
        end
    end

    methods (TestMethodTeardown)
        function closeAll(~)
            omnicore_mex('close_all');
        end
    end

    methods
        function dev = open(tc, varargin)
            dev = omnicore.Device(tc.Serial, 'TxChannels', 1, 'TxGain', 10, varargin{:});
        end
    end

    methods (Static)
        function x = tone(n, channels)
            x = repmat(single(0.5 * exp(1j * 2 * pi * 0.01 * (0:n - 1).')), 1, channels);
        end
    end

    methods (Test)
        function hardwareTimeAdvances(tc)
            dev = tc.open();
            t1 = time(dev);
            k1 = timeTicks(dev);
            pause(0.2);
            t2 = time(dev);
            tc.verifyClass(k1, 'uint64');
            tc.verifyGreaterThan(t2 - t1, 0.15);
            tc.verifyLessThan(t2 - t1, 0.5);
        end

        function transmitOneBurst(tc)
            dev = tc.open();
            s = transmit(dev, TxHardwareTest.tone(100000, 1));
            tc.verifyEqual(s.BurstAck, 1);
            tc.verifyEqual(s.Underflow, 0);
            tc.verifyEqual(s.Late, 0);
            tc.verifyEqual(s.Errors, 0);
            tc.verifyClass(s.AckTicks, 'uint64');
            % A burst shorter than one USB transfer is sent too (started by its end).
            s = transmit(dev, TxHardwareTest.tone(1000, 1));
            tc.verifyEqual(s.BurstAck, 1);
        end

        function timedBurstEndsOnTime(tc)
            dev = tc.open();
            n = 30720;
            t0 = time(dev);
            start = t0 + 0.3;
            s = transmit(dev, TxHardwareTest.tone(n, 1), 'StartTime', start);
            tc.verifyEqual(s.BurstAck, 1);
            tc.verifyEqual(s.Late, 0);
            startTicks = round(start * dev.SampleRate);
            delta = double(s.AckTicks(1)) - startTicks;
            % The acknowledgement carries the hardware time at the end of the burst.
            tc.verifyGreaterThanOrEqual(delta, n - 1);
            tc.verifyLessThanOrEqual(delta, n + 16384, sprintf('ack %g samples after the start', delta));
            tc.verifyGreaterThan(time(dev), start + n / dev.SampleRate, 'transmit returns after the burst');
        end

        function lateBurstWarns(tc)
            dev = tc.open();
            t0 = time(dev);
            s = tc.verifyWarning(@() transmit(dev, TxHardwareTest.tone(20000, 1), 'StartTime', t0 - 1), ...
                                 'omnicore:TxLate');
            tc.verifyGreaterThanOrEqual(s.Late, 1);
        end

        function inputTypesAndShapes(tc)
            dev = tc.open('TxChannels', [1 2]);
            s = transmit(dev, TxHardwareTest.tone(5000, 2));
            tc.verifyEqual(s.BurstAck, 1);
            xi = int16(round(double(TxHardwareTest.tone(5000, 2)) * 32767));
            s = transmit(dev, xi);
            tc.verifyEqual(s.BurstAck, 1);
            tc.verifyWarning(@() transmit(dev, 3 * double(TxHardwareTest.tone(5000, 2))), 'omnicore:TxClipped');
            transmit(dev, real(double(TxHardwareTest.tone(5000, 2)))); % real: Q = 0
            tc.verifyError(@() transmit(dev, TxHardwareTest.tone(100, 1)), 'omnicore:InvalidArgument');
            tc.verifyError(@() transmit(dev, int8(ones(100, 2))), 'omnicore:InvalidArgument');
            tc.verifyError(@() transmit(dev, uint8(ones(100, 2))), 'omnicore:InvalidArgument');
            tc.verifyError(@() transmit(dev, zeros(0, 2)), 'omnicore:InvalidArgument');
            dev.TxChannels = 2;
            s = transmit(dev, TxHardwareTest.tone(5000, 1).'); % row vector on one channel
            tc.verifyEqual(s.BurstAck, 1);
            configure(dev, 'TxChannels', [], 'RxChannels', 1);
            tc.verifyError(@() transmit(dev, TxHardwareTest.tone(100, 1)), 'omnicore:InvalidArgument');
        end

        function eightBitTransmit(tc)
            dev = tc.open('IQBits', 8, 'TxChannels', [1 2]);
            x = int8(round(double(TxHardwareTest.tone(20000, 2)) * 127));
            s = transmit(dev, x);
            tc.verifyEqual(s.BurstAck, 1);
        end

        function streamBursts(tc)
            dev = tc.open();
            tx = txStream(dev);
            tc.verifyFalse(tx.IsActive);
            t0 = time(dev);
            n = 20000;
            for k = 1:3
                write(tx, TxHardwareTest.tone(n, 1), 'StartTime', t0 + 0.2 * k, 'EndBurst', true);
            end
            tc.verifyTrue(tx.IsActive);
            pause(1);
            s = status(tx);
            tc.verifyEqual(s.BurstAck, 3);
            tc.verifyEqual(s.Late, 0);
            tc.verifyEqual(s.Underflow, 0);
            gaps = diff(double(s.AckTicks)) / dev.SampleRate;
            tc.verifyEqual(gaps, [0.2; 0.2], 'AbsTol', 1e-6, 'bursts 0.2 s apart');
            % A burst split over several writes, ended by an empty write.
            write(tx, TxHardwareTest.tone(70000, 1), 'StartTime', time(dev) + 0.2);
            write(tx, TxHardwareTest.tone(70000, 1));
            write(tx, single([]), 'EndBurst', true);
            pause(0.8);
            s = status(tx);
            tc.verifyEqual(s.BurstAck, 1);
            tc.verifyEqual(s.Underflow, 0);
            tc.verifyError(@() write(tx, int16(ones(10, 1))), 'omnicore:InvalidArgument');
            stop(tx);
            tc.verifyFalse(tx.IsActive);
            write(tx, int16(ones(10, 1)), 'EndBurst', true); % another type after stop
            stop(tx);
        end

        function repeatInBackground(tc)
            dev = tc.open();
            transmitRepeat(dev, TxHardwareTest.tone(1000, 1));
            tc.verifyTrue(isTransmitting(dev));
            tc.verifyError(@() transmit(dev, TxHardwareTest.tone(100, 1)), 'omnicore:BUSY');
            tx = txStream(dev);
            tc.verifyError(@() write(tx, TxHardwareTest.tone(100, 1)), 'omnicore:BUSY');
            % RX keeps working alongside, and so does retuning.
            x = receive(dev, 100000);
            tc.verifySize(x, [100000 2]);
            dev.TxFrequency = 1.2e9;
            pause(1);
            transmitRepeat(dev, TxHardwareTest.tone(5000, 1)); % replaces the waveform
            pause(1);
            s = stopTransmit(dev);
            tc.verifyFalse(isTransmitting(dev));
            tc.verifyEqual(s.Underflow, 0);
            tc.verifyEqual(s.Errors, 0);
            tc.verifyEmpty(stopTransmit(dev));
            s = transmit(dev, TxHardwareTest.tone(1000, 1)); % TX slot free again
            tc.verifyEqual(s.BurstAck, 1);
        end

        function restartingOneDirectionNeedsBothStopped(tc)
            % The core resets the USB pipes only when no stream runs: while TX streams, RX can be
            % started once, but not stopped and started again.
            dev = tc.open();
            transmitRepeat(dev, TxHardwareTest.tone(1000, 1));
            x = receive(dev, 1000);
            tc.verifySize(x, [1000 2]);
            tc.verifyError(@() receive(dev, 1000), 'omnicore:BUSY');
            stopTransmit(dev);
            x = receive(dev, 1000); % both stopped: fine again
            tc.verifySize(x, [1000 2]);
        end

        function deleteWhileRepeating(tc)
            dev = tc.open();
            transmitRepeat(dev, TxHardwareTest.tone(1000, 1));
            pause(0.3);
            delete(dev);
            tc.verifyEqual(omnicore_mex('count'), 0);
            tc.verifyFalse(mislocked('omnicore_mex'));
        end

        function timedReceive(tc)
            dev = tc.open();
            start = time(dev) + 0.2;
            [x, meta] = receive(dev, 1000, 'StartTime', start);
            tc.verifySize(x, [1000 2]);
            tc.verifyEqual(meta.TimeTicks, uint64(round(start * dev.SampleRate)), 'first sample at StartTime');
            tc.verifyFalse(meta.Late);
            [~, meta] = receive(dev, 1000, 'StartTime', time(dev) - 1);
            tc.verifyTrue(meta.Late, 'a past StartTime is reported');
        end

        function timedStreamStart(tc)
            dev = tc.open();
            rx = rxStream(dev, 'Timeout', 2);
            t = time(dev) + 0.3;
            rx.start('StartTime', t);
            [~, meta] = read(rx, 1000);
            tc.verifyEqual(meta.TimeTicks, uint64(round(t * dev.SampleRate)));
            stop(rx);
        end
    end
end
