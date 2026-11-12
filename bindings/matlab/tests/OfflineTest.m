classdef OfflineTest < matlab.unittest.TestCase
    %OFFLINETEST Checks that need the MEX file but no device.
    %   Put the staged toolbox folder (MEX file + +omnicore) on the path, then:
    %       results = runtests('OfflineTest')

    methods (TestClassSetup)
        function nothingOpen(tc)
            tc.assumeEqual(omnicore_mex('count'), 0, 'close every omnicore.Device before running the tests');
        end
    end

    methods (Test)
        function versionMatchesHeader(tc)
            v = omnicore.version();
            tc.verifyEqual(v.Abi, 1);
            tc.verifyEqual(v.Library, v.Header, 'MEX file built against a different OmniCore Host');
        end

        function listReturnsStructArray(tc)
            d = omnicore.findDevices();
            tc.verifyTrue(isstruct(d));
            tc.verifyEqual(size(d, 2), 1);
            tc.verifyTrue(all(isfield(d, {'Serial', 'Name', 'Firmware', 'UsbDriver', 'UsbSpeedMbps', ...
                                          'RxChannelCount', 'TxChannelCount'})));
        end

        function unknownCommand(tc)
            tc.verifyError(@() omnicore_mex('no_such_command'), 'omnicore:InvalidArgument');
            tc.verifyError(@() omnicore_mex(42), 'omnicore:InvalidArgument');
            tc.verifyError(@() omnicore_mex(), 'omnicore:InvalidArgument');
        end

        function invalidHandles(tc)
            tc.verifyError(@() omnicore_mex('info', uint64(987654321)), 'omnicore:InvalidHandle');
            tc.verifyError(@() omnicore_mex('config', 1), 'omnicore:InvalidHandle');
            tc.verifyError(@() omnicore_mex('status', uint64([1 2])), 'omnicore:InvalidHandle');
            % Closing an unknown handle is harmless (objects deleted twice).
            omnicore_mex('close', uint64(987654321));
            tc.verifyEqual(omnicore_mex('count'), 0);
        end

        function badRequestStruct(tc)
            tc.verifyError(@() omnicore_mex('open', '', struct('NoSuchField', 1)), 'omnicore:InvalidArgument');
            tc.verifyError(@() omnicore_mex('open', '', struct('SampleRate', 'fast')), 'omnicore:InvalidArgument');
            tc.verifyError(@() omnicore_mex('open', 7), 'omnicore:InvalidArgument');
            tc.verifyEqual(omnicore_mex('count'), 0);
        end

        function deviceRejectsUnknownParameters(tc)
            % Checked before anything is opened.
            tc.verifyError(@() omnicore.Device('NoSuchParameter', 1), 'omnicore:InvalidArgument');
            tc.verifyError(@() omnicore.Device('SERIAL', 'SampleRate'), 'omnicore:InvalidArgument');
            tc.verifyError(@() omnicore.Device(42), 'omnicore:InvalidArgument');
            tc.verifyEqual(omnicore_mex('count'), 0);
        end

        function rxCommandsCheckHandles(tc)
            bad = uint64(987654321);
            tc.verifyError(@() omnicore_mex('receive', bad, 100), 'omnicore:InvalidHandle');
            tc.verifyError(@() omnicore_mex('rx_create', bad), 'omnicore:InvalidHandle');
            tc.verifyError(@() omnicore_mex('rx_start', bad), 'omnicore:InvalidHandle');
            tc.verifyError(@() omnicore_mex('rx_read', bad, 100, 1), 'omnicore:InvalidHandle');
            tc.verifyError(@() omnicore_mex('rx_running', bad), 'omnicore:InvalidHandle');
            omnicore_mex('rx_destroy', bad); % harmless, like close
        end

        function rxStreamNeedsDevice(tc)
            tc.verifyError(@() omnicore.RxStream(42), 'omnicore:InvalidArgument');
            tc.verifyError(@() omnicore.RxStream([]), 'omnicore:InvalidArgument');
        end

        function missingDeviceIsNotFound(tc)
            tc.verifyError(@() omnicore.Device('OmniCoreDOESNOTEXIST'), 'omnicore:NOT_FOUND');
            tc.verifyEqual(omnicore_mex('count'), 0);
            tc.verifyFalse(mislocked('omnicore_mex'));
        end
    end
end
