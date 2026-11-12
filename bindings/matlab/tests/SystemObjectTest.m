classdef (TestTags = {'Hardware'}) SystemObjectTest < matlab.unittest.TestCase
    %SYSTEMOBJECTTEST M4 checks on a connected OmniCoreSDR: omnicore.Receiver / omnicore.Transmitter.
    %   No RF connection is needed (TX at low gain). Skipped when no device is connected.
    %       results = runtests('SystemObjectTest')
    %   The 10-minute full-duplex run and the Simulink model are in hwtest/m4_accept.m.

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

    methods (Static)
        function x = tone(n, channels)
            x = repmat(single(0.5 * exp(1j * 2 * pi * 0.01 * (0:n - 1).')), 1, channels);
        end
    end

    methods (Test)
        function receiverOpensItsOwnDevice(tc)
            rx = omnicore.Receiver('SerialNumber', tc.Serial, 'SamplesPerFrame', 8192, 'ChannelMapping', [1 2], ...
                                   'SampleRate', 30.72e6);
            for k = 1:50
                [y, valid, overflow] = rx();
                tc.assertSize(y, [8192 2]);
                tc.verifyTrue(valid);
                tc.verifyFalse(overflow);
            end
            tc.verifyClass(y, 'single');
            tc.verifyEqual(omnicore_mex('count'), 1);
            release(rx);
            tc.verifyEqual(omnicore_mex('count'), 0, 'release closes a device the object opened');
            y = rx(); % steps again after release: set up anew
            tc.verifySize(y, [8192 1] .* [1 2]);
            release(rx);
        end

        function receiverTunesWhileStreaming(tc)
            dev = omnicore.Device(tc.Serial);
            rx = omnicore.Receiver('Device', dev, 'SamplesPerFrame', 16384, 'OutputDataType', 'int16', ...
                                   'GainMode', 'manual', 'Gain', 30);
            y = rx();
            tc.verifyClass(y, 'int16');
            tc.verifySize(y, [16384 1]);
            tc.verifyEqual(dev.RxChannels, 1);
            rx.CenterFrequency = 1.5e9;
            rx.Gain = 20;
            for k = 1:20
                [~, valid, overflow] = rx();
                tc.verifyTrue(valid);
                tc.verifyFalse(overflow, 'retuning keeps the stream contiguous');
            end
            tc.verifyEqual(dev.RxFrequency, 1.5e9);
            tc.verifyEqual(dev.RxGain, [20 20]);
            release(rx);
            tc.verifyTrue(dev.isOpen(), 'release leaves a given device open');
        end

        function duplexOnOneDevice(tc)
            dev = omnicore.Device(tc.Serial, 'RxChannels', 1, 'TxChannels', 1);
            n = 30720;
            rx = omnicore.Receiver('Device', dev, 'SamplesPerFrame', n);
            tx = omnicore.Transmitter('Device', dev, 'Gain', 10);
            x = SystemObjectTest.tone(n, 1);
            bad = 0;
            for k = 1:1000 % 1 s
                [~, valid, overflow] = rx();
                underflow = tx(x);
                bad = bad + ~valid + overflow + underflow;
            end
            tc.verifyEqual(bad, 0, 'no lost frame, overflow or underflow in 1 s of full duplex');
            tx.Gain = 20;
            tc.verifyEqual(dev.TxGain, [10 10], 'tunable properties apply at the next call');
            tx(x);
            tc.verifyEqual(dev.TxGain, [20 20]);
            release(tx);
            release(rx);
        end

        function duplexBySerialAfterSetup(tc)
            n = 8192;
            rx = omnicore.Receiver('SerialNumber', tc.Serial, 'SamplesPerFrame', n);
            tx = omnicore.Transmitter('SerialNumber', tc.Serial, 'Gain', 10);
            x = SystemObjectTest.tone(n, 1);
            setup(rx);
            setup(tx, x);
            tc.verifyEqual(omnicore_mex('count'), 1, 'one device shared by both objects');
            for k = 1:200
                rx();
                tx(x);
            end
            release(rx);
            tc.verifyEqual(omnicore_mex('count'), 1, 'still held by the Transmitter');
            release(tx);
            tc.verifyEqual(omnicore_mex('count'), 0);
        end

        function channelChangeWhileStreamingIsExplained(tc)
            rx = omnicore.Receiver('SerialNumber', tc.Serial, 'SamplesPerFrame', 4096);
            rx();
            tx = omnicore.Transmitter('SerialNumber', tc.Serial);
            try
                tx(SystemObjectTest.tone(4096, 1));
                tc.verifyFail('expected omnicore:BUSY');
            catch err
                tc.verifyEqual(err.identifier, 'omnicore:BUSY');
                tc.verifySubstring(err.message, 'full duplex');
            end
            release(tx);
            release(rx);
        end

        function invalidProperties(tc)
            tc.verifyError(@() setup(omnicore.Receiver('ChannelMapping', 3)), 'omnicore:InvalidArgument');
            tc.verifyError(@() setup(omnicore.Receiver('SamplesPerFrame', 0)), 'omnicore:InvalidArgument');
            tc.verifyError(@() omnicore.Receiver('OutputDataType', 'uint8'), ?MException);
            tc.verifyError(@() setup(omnicore.Transmitter('Latency', -1), single(1)), 'omnicore:InvalidArgument');
            dev = omnicore.Device(tc.Serial, 'TxChannels', [1 2]);
            tx = omnicore.Transmitter('Device', dev, 'ChannelMapping', [1 2]);
            tc.verifyError(@() tx(single([1; 2])), 'omnicore:InvalidArgument');
        end
    end
end
