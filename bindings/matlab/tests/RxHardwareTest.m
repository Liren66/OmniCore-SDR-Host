classdef (TestTags = {'Hardware'}) RxHardwareTest < matlab.unittest.TestCase
    %RXHARDWARETEST M2 checks on a connected OmniCoreSDR: receive and RxStream (MEX_PLAN.md 8).
    %   No RF connection is needed. Skipped when no device is connected. Run with:
    %       results = runtests('RxHardwareTest')
    %   OMNICORE_TEST_SERIAL selects a device when several are connected. The long acceptance
    %   runs (1e8 samples, 60 s stream) are in hwtest/m2_accept.m.

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

    methods (Test)
        function receiveTwoChannels(tc)
            dev = omnicore.Device(tc.Serial);
            [x, meta] = receive(dev, 1e5);
            tc.verifyClass(x, 'single');
            tc.verifyFalse(isreal(x));
            tc.verifySize(x, [1e5 2]);
            tc.verifyLessThanOrEqual(max(abs([real(x(:)); imag(x(:))])), 1);
            tc.verifyGreaterThan(max(abs(x(:))), 0, 'some noise is expected');
            tc.verifyFalse(meta.Overflow);
            tc.verifySize(meta.Gaps, [0 2]);
            tc.verifyClass(meta.TimeTicks, 'uint64');
            tc.verifyEqual(meta.Time, double(meta.TimeTicks) / dev.SampleRate, 'RelTol', 1e-12);
            tc.verifyFalse(meta.Late);
            % Hardware time moves forward between captures.
            [~, meta2] = receive(dev, 1000);
            tc.verifyGreaterThan(meta2.TimeTicks, meta.TimeTicks + 1e5);
        end

        function receiveOneChannelAndTypes(tc)
            dev = omnicore.Device(tc.Serial, 'RxChannels', 2);
            x = receive(dev, 4096);
            tc.verifySize(x, [4096 1]);
            xi = receive(dev, 4096, 'OutputDataType', 'int16');
            tc.verifyClass(xi, 'int16');
            tc.verifyFalse(isreal(xi));
            tc.verifyEqual(mod(real(xi(1:100)), 16), zeros(100, 1, 'int16'), '12-bit samples left-aligned');
            xd = receive(dev, 4096, 'OutputDataType', 'double');
            tc.verifyClass(xd, 'double');
            tc.verifySize(xd, [4096 1]);
            dev.RxChannels = [1 2];
            xd2 = receive(dev, 5000, 'OutputDataType', 'double');
            tc.verifySize(xd2, [5000 2]);
            tc.verifyError(@() receive(dev, 100, 'OutputDataType', 'int8'), 'omnicore:InvalidArgument');
            tc.verifyError(@() receive(dev, 100, 'OutputDataType', 'uint8'), 'omnicore:InvalidArgument');
        end

        function receiveEightBit(tc)
            dev = omnicore.Device(tc.Serial, 'IQBits', 8);
            x = receive(dev, 8192, 'OutputDataType', 'int8');
            tc.verifyClass(x, 'int8');
            tc.verifySize(x, [8192 2]);
            xs = receive(dev, 8192);
            tc.verifyEqual(xs, single(round(xs * 128)) / 128, 'single from 8-bit samples is k/128');
        end

        function receiveRejectsBadArguments(tc)
            dev = omnicore.Device(tc.Serial);
            tc.verifyError(@() receive(dev, 0), 'omnicore:InvalidArgument');
            tc.verifyError(@() receive(dev, 1.5), 'omnicore:InvalidArgument');
            tc.verifyError(@() receive(dev, 2^33), 'omnicore:InvalidArgument');
            tc.verifyError(@() receive(dev, 10, 'Timeout', 0), 'omnicore:InvalidArgument');
            tc.verifyError(@() receive(dev, 10, 'NoSuchOption', 1), 'omnicore:InvalidArgument');
            configure(dev, 'RxChannels', [], 'TxChannels', 1);
            tc.verifyError(@() receive(dev, 10), 'omnicore:InvalidArgument');
        end

        function streamIsContiguous(tc)
            dev = omnicore.Device(tc.Serial);
            rx = rxStream(dev);
            tc.verifyFalse(rx.IsRunning);
            start(rx);
            tc.verifyTrue(rx.IsRunning);
            n = 65536;
            [~, first] = read(rx, n);
            last = first;
            for k = 1:30
                [x, meta] = read(rx, n);
                tc.assertSize(x, [n 2]);
                tc.verifyFalse(meta.Overflow);
                tc.verifyEqual(meta.TimeTicks, last.TimeTicks + n, 'blocks are back to back');
                last = meta;
            end
            stop(rx);
            tc.verifyFalse(rx.IsRunning);
            tc.verifyError(@() read(rx, 10), 'omnicore:STOPPED');
            % A stream starts again after stop.
            start(rx);
            x = read(rx, 1000);
            tc.verifySize(x, [1000 2]);
        end

        function pauseShowsAsGap(tc)
            dev = omnicore.Device(tc.Serial);
            rx = rxStream(dev, 'BufferBytes', 16 * 2^20, 'OutputDataType', 'int16');
            start(rx);
            n = 65536;
            [~, a] = read(rx, n);
            pause(1); % 16 MiB holds ~0.09 s at 2 x 30.72 MSPS (3 bytes per sample): the rest is lost
            [~, b] = read(rx, n);
            got = 0;
            while ~b.Overflow && got < 200
                % the ring buffer still held samples from before the loss
                c = b;
                [~, b] = read(rx, n);
                got = got + 1;
            end
            tc.verifyTrue(b.Overflow, 'a 1 s pause overflows a 16 MiB buffer');
            tc.verifyGreaterThanOrEqual(size(b.Gaps, 1), 1);
            lost = sum(b.Gaps(:, 2));
            tc.verifyGreaterThan(lost, 0.3 * dev.SampleRate);
            if got == 0
                before = a.TimeTicks + n;
            else
                before = c.TimeTicks + n;
            end
            if b.Gaps(1, 1) == 1
                % Lost before this read: the read starts after the gap.
                tc.verifyEqual(double(b.TimeTicks - before), b.Gaps(1, 2));
            else
                tc.verifyEqual(b.TimeTicks, before);
            end
        end

        function retuneWhileStreaming(tc)
            dev = omnicore.Device(tc.Serial, 'RxGainMode', 'manual');
            rx = rxStream(dev);
            start(rx);
            read(rx, 65536);
            dev.RxFrequency = 1.1e9;
            dev.RxGain = 20;
            reconfigured = false;
            for k = 1:20
                [~, meta] = read(rx, 65536);
                reconfigured = reconfigured || meta.Reconfigured;
                tc.verifyFalse(meta.Overflow, 'a retune keeps the stream contiguous');
            end
            tc.verifyTrue(reconfigured);
        end

        function oneRxStreamPerDevice(tc)
            dev = omnicore.Device(tc.Serial);
            rx = rxStream(dev);
            start(rx);
            tc.verifyError(@() receive(dev, 100), 'omnicore:BUSY');
            rx2 = rxStream(dev);
            tc.verifyError(@() start(rx2), 'omnicore:BUSY');
            tc.verifyError(@() start(rx), 'omnicore:BUSY');
            tc.verifyError(@() HardwareTest_assign(dev, 'RxChannels', 1), 'omnicore:BUSY');
            stop(rx);
            x = receive(dev, 100);
            tc.verifySize(x, [100 2]);
        end

        function closingDeviceEndsStream(tc)
            dev = omnicore.Device(tc.Serial);
            rx = rxStream(dev);
            start(rx);
            delete(dev);
            tc.verifyEqual(omnicore_mex('count'), 0);
            tc.verifyFalse(rx.IsRunning);
            tc.verifyError(@() read(rx, 10), 'omnicore:InvalidHandle');
            delete(rx); % harmless
            tc.verifyFalse(mislocked('omnicore_mex'));
        end

        function clearingStreamStopsIt(tc)
            dev = omnicore.Device(tc.Serial);
            rx = rxStream(dev);
            start(rx);
            clear rx
            x = receive(dev, 100); % the RX slot is free again
            tc.verifySize(x, [100 2]);
        end
    end
end

function HardwareTest_assign(obj, name, value)
obj.(name) = value;
end
