classdef Receiver < matlab.System & matlab.system.mixin.Propagates & matlab.system.mixin.SampleTime
    %RECEIVER Frame-based reception from an OmniCoreSDR (System object; MATLAB System block).
    %   RX = omnicore.Receiver(Name, Value, ...) and then, in a loop,
    %       [Y, VALID, OVERFLOW] = RX();
    %   Y is a SamplesPerFrame x numel(ChannelMapping) complex frame. VALID is false when the frame
    %   was not filled within Timeout (the rest is zeros). OVERFLOW is true when samples were lost
    %   before or inside the frame (MATLAB fell behind; see omnicore.RxStream).
    %
    %   Properties (set before the first call unless marked tunable; a tunable change applies at
    %   the next call):
    %     Device           an omnicore.Device to use (shared with other objects); otherwise the
    %                      object opens SerialNumber itself and closes it on release
    %     SerialNumber     '' = the only connected device
    %     SamplesPerFrame  default 4096
    %     OutputDataType   'single' (default, +-1), 'double', 'int16' or 'int8' (IQBits 8)
    %     ChannelMapping   RX channels, 1, 2 or [1 2]; sets Device.RxChannels. Default 1
    %     SampleRate       Hz; NaN (default) keeps the device's rate
    %     CenterFrequency  Hz, tunable; NaN keeps the device's value
    %     Gain             dB, tunable (scalar or per channel); NaN keeps the
    %                      device's value
    %     GainMode         'manual', 'fast', 'slow' or 'hybrid', tunable; '' keeps it
    %     BufferBytes      host ring buffer; NaN = 64 MiB
    %     Timeout          seconds to wait for each block, default 1
    %
    %   Streaming starts on the first call and stops on release(RX). Tunable properties apply while
    %   streaming without stopping it (OVERFLOW stays false; the frame after a retune may contain
    %   the PLL transient).
    %
    %   Full duplex with omnicore.Transmitter: either pass both objects one Device opened with
    %   RxChannels and TxChannels, or set both up (setup(RX), setup(TX, x)) before the first call,
    %   because channels cannot change while a stream runs. In Simulink every block is set up
    %   before the simulation starts, so two blocks with the same SerialNumber work as they are.
    %
    %   Example:
    %       rx = omnicore.Receiver('CenterFrequency', 2.45e9, 'SampleRate', 30.72e6, ...
    %                              'SamplesPerFrame', 65536);
    %       for k = 1:100
    %           [y, valid, overflow] = rx();
    %       end
    %       release(rx);
    %
    %   See also omnicore.Transmitter, omnicore.Device, omnicore.RxStream.

    properties (Nontunable)
        SerialNumber = ''
        SamplesPerFrame = 4096
        OutputDataType = 'single'
        ChannelMapping = 1
        SampleRate = NaN
        BufferBytes = NaN
        Timeout = 1
    end

    properties
        CenterFrequency = NaN
        Gain = NaN
        GainMode = ''
    end

    properties (Nontunable, Hidden)
        Device = []
    end

    properties (Constant, Hidden)
        OutputDataTypeSet = matlab.system.StringSet({'single', 'double', 'int16', 'int8'})
    end

    properties (Access = private)
        Dev = []
        Stream = []
        OwnsDevice = false
    end

    methods
        function obj = Receiver(varargin)
            setProperties(obj, nargin, varargin{:});
        end
    end

    methods (Access = protected)
        function validatePropertiesImpl(obj)
            n = obj.SamplesPerFrame;
            if ~(isnumeric(n) && isscalar(n) && n >= 1 && n == round(n))
                error('omnicore:InvalidArgument', 'SamplesPerFrame must be a positive whole number');
            end
            c = obj.ChannelMapping;
            if ~(isequal(c, 1) || isequal(c, 2) || isequal(c(:).', [1 2]))
                error('omnicore:InvalidArgument', 'ChannelMapping must be 1, 2 or [1 2]');
            end
            if ~isempty(obj.Device) && ~(isa(obj.Device, 'omnicore.Device') && isscalar(obj.Device))
                error('omnicore:InvalidArgument', 'Device must be an omnicore.Device');
            end
        end

        function setupImpl(obj)
            args = {'RxChannels', obj.ChannelMapping(:).'};
            args = [args, omnicore.Receiver.optional('SampleRate', obj.SampleRate), ...
                    omnicore.Receiver.optional('RxFrequency', obj.CenterFrequency), ...
                    omnicore.Receiver.optional('RxGainMode', obj.GainMode), ...
                    omnicore.Receiver.optional('RxGain', obj.Gain)];
            obj.OwnsDevice = isempty(obj.Device);
            try
                if obj.OwnsDevice
                    obj.Dev = omnicore.Device(char(obj.SerialNumber), args{:});
                else
                    obj.Dev = obj.Device;
                    if isequal(obj.Dev.RxChannels, obj.ChannelMapping(:).')
                        args = args(3:end); % unchanged channels: no mask change while streaming
                    end
                    if ~isempty(args)
                        configure(obj.Dev, args{:});
                    end
                end
            catch err
                omnicore.Receiver.explainBusy(err);
            end
            opts = {'OutputDataType', obj.OutputDataType, 'Timeout', obj.Timeout};
            if ~isnan(obj.BufferBytes)
                opts = [opts, {'BufferBytes', obj.BufferBytes}];
            end
            obj.Stream = rxStream(obj.Dev, opts{:});
        end

        function [y, valid, overflow] = stepImpl(obj)
            if ~obj.Stream.IsRunning
                start(obj.Stream);
            end
            n = obj.SamplesPerFrame;
            try
                [y, meta] = read(obj.Stream, n);
                overflow = meta.Overflow;
            catch err
                if ~strcmp(err.identifier, 'omnicore:TIMEOUT')
                    rethrow(err);
                end
                y = zeros(0, numel(obj.ChannelMapping), obj.OutputDataType);
                overflow = false;
            end
            valid = size(y, 1) == n;
            if ~valid
                y = [y; complex(zeros(n - size(y, 1), numel(obj.ChannelMapping), obj.OutputDataType))];
            end
        end

        function processTunedPropertiesImpl(obj)
            args = {};
            if isChangedProperty(obj, 'CenterFrequency')
                args = [args, omnicore.Receiver.optional('RxFrequency', obj.CenterFrequency)];
            end
            if isChangedProperty(obj, 'GainMode')
                args = [args, omnicore.Receiver.optional('RxGainMode', obj.GainMode)];
            end
            if isChangedProperty(obj, 'Gain')
                args = [args, omnicore.Receiver.optional('RxGain', obj.Gain)];
            end
            if ~isempty(args)
                configure(obj.Dev, args{:});
            end
        end

        function releaseImpl(obj)
            if ~isempty(obj.Stream) && isvalid(obj.Stream)
                delete(obj.Stream);
            end
            obj.Stream = [];
            if obj.OwnsDevice && ~isempty(obj.Dev) && isvalid(obj.Dev)
                delete(obj.Dev);
            end
            obj.Dev = [];
        end

        function n = getNumInputsImpl(~)
            n = 0;
        end

        function n = getNumOutputsImpl(~)
            n = 3;
        end

        function [a, b, c] = getOutputSizeImpl(obj)
            a = [obj.SamplesPerFrame, numel(obj.ChannelMapping)];
            b = [1 1];
            c = [1 1];
        end

        function [a, b, c] = getOutputDataTypeImpl(obj)
            a = obj.OutputDataType;
            b = 'logical';
            c = 'logical';
        end

        function [a, b, c] = isOutputComplexImpl(~)
            a = true;
            b = false;
            c = false;
        end

        function [a, b, c] = isOutputFixedSizeImpl(~)
            a = true;
            b = true;
            c = true;
        end

        function st = getSampleTimeImpl(obj)
            if isnan(obj.SampleRate)
                st = createSampleTime(obj, 'Type', 'Inherited');
            else
                st = createSampleTime(obj, 'Type', 'Discrete', 'SampleTime', obj.SamplesPerFrame / obj.SampleRate);
            end
        end
    end

    methods (Static, Access = protected)
        function simMode = getSimulateUsingImpl
            simMode = 'Interpreted execution';
        end

        function tf = showSimulateUsingImpl
            tf = false;
        end
    end

    methods (Static, Hidden)
        function args = optional(name, value)
            % Name-value pair for configure, or nothing when the property keeps the device value
            % (NaN, or '' for GainMode).
            if isempty(value) || (isnumeric(value) && all(isnan(value)))
                args = {};
            else
                args = {name, value};
            end
        end

        function explainBusy(err)
            if strcmp(err.identifier, 'omnicore:BUSY')
                error('omnicore:BUSY', ['%s\nChannels cannot change while the device streams. For full ' ...
                       'duplex, set up the Receiver and Transmitter before the first call, or pass both ' ...
                       'one omnicore.Device opened with RxChannels and TxChannels.'], err.message);
            end
            rethrow(err);
        end
    end
end
