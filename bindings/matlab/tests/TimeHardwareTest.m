classdef (TestTags = {'Hardware'}) TimeHardwareTest < matlab.unittest.TestCase
    %TIMEHARDWARETEST M5 checks on a connected OmniCoreSDR: clock and time sources, setTime, PPS,
    %   events and the sample-rate warning (MEX_PLAN.md 8). Uses the board's internal PPS; no
    %   external 10 MHz or PPS is needed (see hwtest/m5_accept.m for those). Leaves ClockSource
    %   'internal' and TimeSource 'none'.
    %       results = runtests('TimeHardwareTest')

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
        function restore(tc)
            omnicore_mex('close_all');
            dev = omnicore.Device(tc.Serial);
            dev.TimeSource = 'none';
            dev.ClockSource = 'internal';
            delete(dev);
        end
    end

    methods (Test)
        function sources(tc)
            dev = omnicore.Device(tc.Serial);
            dev.ClockSource = 'internal';
            tc.verifyEqual(dev.ClockSource, 'internal');
            for s = {'internal', 'none'}
                dev.TimeSource = s{1};
                tc.verifyEqual(dev.TimeSource, s{1});
            end
            tc.verifyError(@() TimeHardwareTest.assign(dev, 'ClockSource', 'gps'), 'omnicore:InvalidArgument');
            tc.verifyError(@() TimeHardwareTest.assign(dev, 'TimeSource', 'gps'), 'omnicore:InvalidArgument');
        end

        function setTimeNow(tc)
            dev = omnicore.Device(tc.Serial);
            setTime(dev, 1000);
            t = time(dev);
            tc.verifyGreaterThanOrEqual(t, 1000);
            tc.verifyLessThan(t, 1000.2);
            tc.verifyError(@() setTime(dev, -1), 'omnicore:InvalidArgument');
            tc.verifyError(@() setTime(dev, 0, 'NoSuch', 1), 'omnicore:InvalidArgument');
        end

        function ppsAlignsTheTime(tc)
            dev = omnicore.Device(tc.Serial);
            fs = dev.SampleRate;
            dev.TimeSource = 'internal';
            pause(1.2);
            setTime(dev, 0, 'AtNextPPS', true);
            pause(2.3); % past the load edge and the edge after it
            s = omnicore_mex('time', TimeHardwareTest.idOf(dev));
            tc.verifyTrue(s.Valid);
            tc.verifyEqual(s.Source, 'internal');
            [t, ticks] = lastPPS(dev);
            % The PPS is not synchronous to the sample clock: its edge latches with +-1 sample.
            off = double(mod(ticks + 1, uint64(fs))) - 1;
            tc.verifyLessThanOrEqual(abs(off), 1, sprintf('PPS edge %d samples from a whole second', off));
            tc.verifyEqual(t, round(t), 'AbsTol', 1.1 / fs);
            % A capture starting on a whole second starts on a PPS edge.
            start = ceil(time(dev)) + 1;
            [~, meta] = receive(dev, 1000, 'StartTime', start, 'Timeout', 2);
            tc.verifyEqual(meta.TimeTicks, uint64(start * fs));
            tc.verifyEqual(meta.Time, start);
        end

        function rateChangeWarnsOnlyWhenTimeWasSet(tc)
            dev = omnicore.Device(tc.Serial);
            % The time stays valid across opens: one rate change makes sure it is not.
            state = warning('off', 'omnicore:TimeInvalidated');
            dev.SampleRate = 25e6;
            warning(state);
            events(dev); % drain
            tc.verifyWarningFree(@() TimeHardwareTest.assign(dev, 'SampleRate', 20e6));
            setTime(dev, 0);
            tc.verifyWarning(@() TimeHardwareTest.assign(dev, 'SampleRate', 30.72e6), 'omnicore:TimeInvalidated');
            pause(0.3);
            e = events(dev);
            tc.verifyTrue(any(strcmp({e.Type}, 'time_invalidated')));
        end

        function eventsDrain(tc)
            dev = omnicore.Device(tc.Serial);
            dev.SampleRate = 20e6;
            pause(0.3);
            e = events(dev);
            tc.verifyTrue(isstruct(e));
            tc.verifyTrue(all(isfield(e, {'Type', 'Message', 'HwTime', 'HwTicks', 'Count', 'HostTime'})));
            tc.verifyEqual(size(e, 2), 1);
            tc.verifyEmpty(events(dev), 'events returns each event once');
        end
    end

    methods (Static, Access = private)
        function assign(obj, name, value)
            obj.(name) = value;
        end

        function id = idOf(dev)
            warning('off', 'MATLAB:structOnObject');
            s = struct(dev);
            id = s.Id;
        end
    end
end
