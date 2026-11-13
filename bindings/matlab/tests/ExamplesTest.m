classdef ExamplesTest < matlab.unittest.TestCase
    %EXAMPLESTEST The application examples in simulation, and the documentation pages. No device.
    %   Put the staged toolbox folder on the path (the examples folder is found next to the MEX
    %   file), then:
    %       results = runtests('ExamplesTest')

    properties
        Folder
    end

    methods (TestClassSetup)
        function addExamples(tc)
            root = fileparts(which('omnicore_mex'));
            tc.assumeNotEmpty(root, 'omnicore_mex is not on the path');
            tc.Folder = fullfile(root, 'examples');
            tc.applyFixture(matlab.unittest.fixtures.PathFixture(tc.Folder));
            tc.addTeardown(@() close(findall(0, 'Type', 'figure')));
        end
    end

    methods (Test)
        function fmRadioRecoversTheStation(tc)
            a = fm_radio(98.7e6, 'Simulate', true, 'Seconds', 1.5, 'Audio', false, 'Plot', false);
            tc.verifyEqual(numel(a), 38 * 1920);       % whole 40 ms blocks: 1.5 s -> 38 blocks at 48 kHz
            s = load('handel.mat');
            ref = interp1((0:numel(s.y) - 1).' / s.Fs, s.y, (0:numel(a) - 1).' / 48e3, 'linear', 0);
            seg = 30001:numel(a);
            c = arrayfun(@(lag) ExamplesTest.correlation(a(seg), ref(seg - lag)), 0:40);
            tc.verifyGreaterThan(max(c), 0.95, 'demodulated audio does not match the source');
        end

        function fmLoopbackMeasuresTheLink(tc)
            r = fm_loopback('Simulate', true, 'Seconds', 2, 'Audio', false, 'Plot', false);
            tc.verifyEqual(r.DelaySeconds, 0.2, 'AbsTol', 0.01);  % the 200 ms TX margin
            tc.verifyGreaterThan(r.SnrDb, 15);
            tc.verifyEqual(r.Lost, 0);
        end

        function videoLinkDecodesEveryFrame(tc)
            r = video_link('Simulate', true, 'Seconds', 2, 'Plot', false, 'Video', '');
            tc.verifyGreaterThan(r.Sent, 3);
            tc.verifyEqual(r.Received, r.Sent);
            tc.verifyEqual(r.CfoHz, 300, 'AbsTol', 20);         % the simulated LO offset
            tc.verifyEqual(r.DelaySamples, 3000);                % the simulated channel delay
            tc.verifyLessThan(r.EvmDb, -20);
        end

        function videoLinkRejectsCorruptFrames(tc)
            r = video_link('Simulate', true, 'Seconds', 1, 'Plot', false, 'Video', '', ...
                           'Channel', struct('Loss', 68));
            tc.verifyEqual(r.Received, 0);
            f = struct2cell(r.Failed);
            tc.verifyEqual(sum([f{:}]), r.Sent, 'every lost frame is counted under one reason');
        end

        function scopeRuns(tc)
            sdr_scope('Simulate', true, 'Seconds', 1);
            tc.verifyEmpty(findall(0, 'Type', 'figure', 'Name', 'OmniCoreSDR scope (simulated)'));
        end

        function examplesNeedOnlyMatlab(tc)
            for name = {'fm_radio', 'fm_loopback', 'video_link', 'sdr_scope'}
                [~, products] = matlab.codetools.requiredFilesAndProducts(fullfile(tc.Folder, [name{1}, '.m']));
                tc.verifyEqual({products.Name}, {'MATLAB'}, name{1});
            end
        end

        function docPagesExist(tc)
            for topic = {'', 'Device', 'omnicore.RxStream', 'receive', 'setTime', 'examples', 'errors'}
                if isempty(topic{1})
                    f = omnicore.doc();
                else
                    f = omnicore.doc(topic{1});
                end
                tc.verifyEqual(exist(f, 'file'), 2, topic{1});
            end
            tc.verifyError(@() omnicore.doc('no_such_page'), 'omnicore:InvalidArgument');
            tc.verifyEqual(exist(fullfile(fileparts(which('omnicore_mex')), 'info.xml'), 'file'), 2);
        end
    end

    methods (Static)
        function c = correlation(a, b)
            c = abs(a' * b) / sqrt((a' * a) * (b' * b));
        end
    end
end
