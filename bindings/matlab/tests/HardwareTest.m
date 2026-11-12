classdef (TestTags = {'Hardware'}) HardwareTest < matlab.unittest.TestCase
    %HARDWARETEST M1 checks on a connected OmniCoreSDR (MEX_PLAN.md section 8).
    %   No RF connection is needed. Skipped when no device is connected. Run with:
    %       results = runtests('HardwareTest')
    %   OMNICORE_TEST_SERIAL selects a device when several are connected.

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
        function openAndClose(tc)
            dev = omnicore.Device(tc.Serial);
            tc.verifyEqual(dev.Serial, tc.Serial);
            tc.verifyTrue(dev.isOpen());
            tc.verifyEqual(omnicore_mex('count'), 1);
            tc.verifyTrue(mislocked('omnicore_mex'), 'MEX file must stay loaded while a device is open');
            info = dev.info();
            tc.verifyEqual(info.RxChannelCount, 2);
            tc.verifyEqual(info.TxChannelCount, 2);
            tc.verifyTrue(info.HardwareTimestamps);
            delete(dev);
            tc.verifyEqual(omnicore_mex('count'), 0);
            tc.verifyFalse(mislocked('omnicore_mex'));
        end

        function defaultsAfterOpen(tc)
            dev = omnicore.Device(tc.Serial);
            tc.verifyEqual(dev.SampleRate, 30.72e6);
            tc.verifyEqual(dev.RxFrequency, 1e9);
            tc.verifyEqual(dev.RxChannels, [1 2]);
            tc.verifyEmpty(dev.TxChannels);
            tc.verifyEqual(dev.RxGainMode, 'slow');
            tc.verifyEqual(dev.IQBits, 12);
        end

        function openWithOptions(tc)
            dev = omnicore.Device(tc.Serial, 'TxChannels', 1, 'RxChannels', 2, 'RxFrequency', 2.45e9);
            tc.verifyEqual(dev.TxChannels, 1);
            tc.verifyEqual(dev.RxChannels, 2);
            tc.verifyEqual(dev.RxFrequency, 2.45e9);
        end

        function secondOpenSharesTheDevice(tc)
            a = omnicore.Device(tc.Serial);
            b = omnicore.Device(tc.Serial, 'RxFrequency', 900e6);
            tc.verifyEqual(omnicore_mex('count'), 1, 'one och_device per serial');
            tc.verifyEqual(a.RxFrequency, 900e6, 'both objects see the same device');
            delete(a);
            tc.verifyTrue(b.isOpen());
            tc.verifyEqual(b.RxFrequency, 900e6, 'still open through the second object');
            delete(b);
            tc.verifyEqual(omnicore_mex('count'), 0);
        end

        function openWithoutSerialFindsOpenDevice(tc)
            tc.assumeEqual(numel(omnicore.findDevices()), 1, 'needs exactly one connected device');
            a = omnicore.Device();
            b = omnicore.Device();
            tc.verifyEqual(omnicore_mex('count'), 1);
            tc.verifyEqual(b.Serial, a.Serial);
        end

        function rfProperties(tc)
            dev = omnicore.Device(tc.Serial);
            dev.RxFrequency = 2.45e9;
            dev.TxFrequency = 2.40e9;
            dev.RxBandwidth = 20e6;
            dev.TxBandwidth = 10e6;
            dev.RxGainMode = 'manual';
            dev.RxGain = [40 20];
            dev.TxGain = 80;
            tc.verifyEqual(dev.RxFrequency, 2.45e9);
            tc.verifyEqual(dev.TxFrequency, 2.40e9);
            tc.verifyEqual(dev.RxBandwidth, 20e6, 'AbsTol', 1);
            tc.verifyEqual(dev.TxBandwidth, 10e6, 'AbsTol', 1);
            tc.verifyEqual(dev.RxGainMode, 'manual');
            tc.verifyEqual(dev.RxGain, [40 20]);
            tc.verifyEqual(dev.TxGain, [80 80]);
            tc.verifyEqual(dev.TxAttenuation, [9.75 9.75]);
            dev.TxGain = [10.1 0];
            tc.verifyEqual(dev.TxGain, [10 0], '0.25 dB steps');
        end

        function configureAppliesTogether(tc)
            dev = omnicore.Device(tc.Serial);
            configure(dev, 'SampleRate', 61.44e6, 'RxBandwidth', 56e6, 'TxBandwidth', 56e6, ...
                      'RxFrequency', 3.5e9, 'RxGainMode', 'manual', 'RxGain', 30);
            tc.verifyEqual(dev.SampleRate, 61.44e6);
            tc.verifyEqual(dev.RxBandwidth, 56e6, 'AbsTol', 1);
            tc.verifyEqual(dev.RxGain, [30 30]);
            % A rate the clock chain cannot hit exactly is rounded by the device.
            dev.SampleRate = 10e6 + 1;
            tc.verifyEqual(dev.SampleRate, 10e6 + 1, 'AbsTol', 10);
        end

        function gainFollowsBand(tc)
            dev = omnicore.Device(tc.Serial, 'RxFrequency', 1e9, 'RxGainMode', 'manual', 'RxGain', 73);
            tc.verifyEqual(dev.RxGain, [73 73]);
            tc.verifyWarning(@() HardwareTest.assign(dev, 'RxFrequency', 5.8e9), 'omnicore:GainLimited');
            tc.verifyEqual(dev.RxGain, [62 62]);
            tc.verifyWarning(@() HardwareTest.assign(dev, 'RxGain', [70 10]), 'omnicore:GainLimited');
            tc.verifyEqual(dev.RxGain, [62 10]);
        end

        function invalidValuesLeaveConfigUnchanged(tc)
            dev = omnicore.Device(tc.Serial);
            before = omnicore_mex('config', tc.idOf(dev));
            tc.verifyError(@() HardwareTest.assign(dev, 'SampleRate', 1e6), 'omnicore:InvalidArgument');
            tc.verifyError(@() HardwareTest.assign(dev, 'RxFrequency', 7e9), 'omnicore:InvalidArgument');
            tc.verifyError(@() HardwareTest.assign(dev, 'TxGain', -1), 'omnicore:InvalidArgument');
            tc.verifyError(@() HardwareTest.assign(dev, 'RxChannels', 3), 'omnicore:InvalidArgument');
            tc.verifyError(@() HardwareTest.assign(dev, 'RxGainMode', 'auto'), 'omnicore:InvalidArgument');
            tc.verifyError(@() configure(dev, 'RxFrequency', 2e9, 'SampleRate', 0), 'omnicore:InvalidArgument');
            tc.verifyEqual(omnicore_mex('config', tc.idOf(dev)), before);
        end

        function eightBitMode(tc)
            dev = omnicore.Device(tc.Serial, 'IQBits', 8);
            tc.verifyEqual(dev.IQBits, 8);
            tc.verifyEqual(dev.RxChannels, [1 2]);
            tc.verifyEqual(dev.TxChannels, [1 2]);
            tc.verifyError(@() HardwareTest.assign(dev, 'TxChannels', 1), 'omnicore:InvalidArgument');
            configure(dev, 'IQBits', 12, 'TxChannels', []);
            tc.verifyEqual(dev.IQBits, 12);
            tc.verifyEmpty(dev.TxChannels);
        end

        function statusFields(tc)
            dev = omnicore.Device(tc.Serial);
            s = dev.status();
            tc.verifyTrue(all(isfield(s, {'Temperature', 'RefLocked', 'PpsLocked', 'VcxoLocked', 'TimeValid', ...
                                          'UsbSpeedMbps', 'RxRssi'})));
            tc.verifyTrue(isnan(s.Temperature) || (s.Temperature > 0 && s.Temperature < 120));
            tc.verifySize(s.RxRssi, [1 2]);
            tc.verifyTrue(islogical(s.RefLocked));
        end

        function boardSettings(tc)
            dev = omnicore.Device(tc.Serial);
            rgb = dev.RgbEnable;
            tc.addTeardown(@() HardwareTest.assign(dev, 'RgbEnable', rgb));
            dev.RgbEnable = ~rgb;
            tc.verifyEqual(dev.RgbEnable, ~rgb);
            dev.RgbEnable = rgb;
            tc.verifyEqual(dev.RgbEnable, rgb);
            tc.verifyTrue(any(strcmp(dev.RxLOSource, {'internal', 'external'})));
            tc.verifyClass(dev.RssiEnable, 'logical');
            tc.verifyError(@() HardwareTest.assign(dev, 'RxLOSource', 'both'), 'omnicore:InvalidArgument');
        end

        function closedObject(tc)
            dev = omnicore.Device(tc.Serial);
            omnicore_mex('close_all');
            tc.verifyError(@() dev.SampleRate, 'omnicore:InvalidHandle');
            delete(dev); % harmless after close_all
        end

        function clearMexKeepsDeviceUsable(tc)
            dev = omnicore.Device(tc.Serial);
            clear mex %#ok<CLMEX> the MEX file is locked while a device is open
            tc.verifyTrue(mislocked('omnicore_mex'));
            dev.RxFrequency = 1.5e9;
            tc.verifyEqual(dev.RxFrequency, 1.5e9);
        end
    end

    methods (Static, Access = private)
        function assign(obj, name, value)
            % Property assignment as a function, for verifyError / verifyWarning / teardown.
            obj.(name) = value;
        end
    end

    methods (Access = private)
        function id = idOf(~, dev)
            % Tests only: the private handle id, for comparing raw configuration structs.
            warning('off', 'MATLAB:structOnObject');
            s = struct(dev);
            id = s.Id;
        end
    end
end
