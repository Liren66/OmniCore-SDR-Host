classdef Transmitter < matlab.System & matlab.system.mixin.Propagates
    %TRANSMITTER Frame-based transmission to an OmniCoreSDR (System object; MATLAB System block).
    %   TX = omnicore.Transmitter(Name, Value, ...) and then, in a loop,
    %       UNDERFLOW = TX(X);
    %   X is an N x numel(ChannelMapping) frame: complex single or double scaled to +-1, int16, or
    %   int8 (IQBits 8). The frames form one continuous burst; TX(X) returns once X is queued.
    %   UNDERFLOW is true when the device ran out of samples since the previous call: frames must
    %   arrive at least as fast as the sample rate.
    %
    %   Properties (set before the first call unless marked tunable; a tunable change applies at
    %   the next call):
    %     Device           an omnicore.Device to use (shared with other objects); otherwise the
    %                      object opens SerialNumber itself and closes it on release
    %     SerialNumber     '' = the only connected device
    %     ChannelMapping   TX channels, 1, 2 or [1 2]; sets Device.TxChannels. Default 1
    %     SampleRate       Hz; NaN (default) keeps the device's rate
    %     CenterFrequency  Hz, tunable; NaN keeps the device's value. A TX LO change briefly
    %                      pauses streaming (the core discards queued TX samples)
    %     Gain             dB 0..89.75, tunable (scalar or per channel); NaN keeps the
    %                      device's value
    %     Latency          seconds of zeros sent before the first frame, default 0.1: how far
    %                      the device runs behind the writes, and so how long MATLAB may pause
    %                      before an underflow. The first Simulink run after starting MATLAB pauses
    %                      longer than that once (compilation); later runs do not
    %     BufferBytes      host ring buffer; NaN = 64 MiB
    %     Timeout          seconds a call may wait for buffer space, default 1
    %
    %   release(TX) sends what is queued and stops. Full duplex: see omnicore.Receiver.
    %
    %   Example (a tone, rate-matched to a Receiver):
    %       dev = omnicore.Device('RxChannels', 1, 'TxChannels', 1, 'TxFrequency', 2.45e9);
    %       rx = omnicore.Receiver('Device', dev, 'SamplesPerFrame', 30720);
    %       tx = omnicore.Transmitter('Device', dev, 'Gain', 30);
    %       x = single(0.5 * exp(1j * 2 * pi * (0:30719).' / 30.72));
    %       for k = 1:1000
    %           y = rx();
    %           tx(x);
    %       end
    %       release(rx); release(tx);
    %
    %   See also omnicore.Receiver, omnicore.Device, omnicore.TxStream.

    properties (Nontunable)
        SerialNumber = ''
        ChannelMapping = 1
        SampleRate = NaN
        Latency = 0.1
        BufferBytes = NaN
        Timeout = 1
    end

    properties
        CenterFrequency = NaN
        Gain = NaN
    end

    properties (Nontunable, Hidden)
        Device = []
    end

    properties (Access = private)
        Dev = []
        Stream = []
        OwnsDevice = false
        Started = false
    end

    methods
        function obj = Transmitter(varargin)
            setProperties(obj, nargin, varargin{:});
        end
    end

    methods (Access = protected)
        function validatePropertiesImpl(obj)
            c = obj.ChannelMapping;
            if ~(isequal(c, 1) || isequal(c, 2) || isequal(c(:).', [1 2]))
                error('omnicore:InvalidArgument', 'ChannelMapping must be 1, 2 or [1 2]');
            end
            if ~(isnumeric(obj.Latency) && isscalar(obj.Latency) && obj.Latency >= 0 && obj.Latency <= 10)
                error('omnicore:InvalidArgument', 'Latency must be 0 to 10 seconds');
            end
            if ~isempty(obj.Device) && ~(isa(obj.Device, 'omnicore.Device') && isscalar(obj.Device))
                error('omnicore:InvalidArgument', 'Device must be an omnicore.Device');
            end
        end

        function validateInputsImpl(obj, x)
            if ~(isnumeric(x) && ismatrix(x) && size(x, 2) == numel(obj.ChannelMapping))
                error('omnicore:InvalidArgument', 'Transmitter input must have %d column(s), one per channel', ...
                      numel(obj.ChannelMapping));
            end
        end

        function setupImpl(obj, ~)
            args = {'TxChannels', obj.ChannelMapping(:).'};
            args = [args, omnicore.Receiver.optional('SampleRate', obj.SampleRate), ...
                    omnicore.Receiver.optional('TxFrequency', obj.CenterFrequency), ...
                    omnicore.Receiver.optional('TxGain', obj.Gain)];
            obj.OwnsDevice = isempty(obj.Device);
            try
                if obj.OwnsDevice
                    obj.Dev = omnicore.Device(char(obj.SerialNumber), args{:});
                else
                    obj.Dev = obj.Device;
                    if isequal(obj.Dev.TxChannels, obj.ChannelMapping(:).')
                        args = args(3:end);
                    end
                    if ~isempty(args)
                        configure(obj.Dev, args{:});
                    end
                end
            catch err
                omnicore.Receiver.explainBusy(err);
            end
            opts = {'Timeout', obj.Timeout};
            if ~isnan(obj.BufferBytes)
                opts = [opts, {'BufferBytes', obj.BufferBytes}];
            end
            obj.Stream = txStream(obj.Dev, opts{:});
            obj.Started = false;
        end

        function underflow = stepImpl(obj, x)
            if ~obj.Started
                lead = round(obj.Latency * obj.Dev.SampleRate);
                if lead > 0
                    write(obj.Stream, zeros(lead, size(x, 2), 'like', x));
                end
                obj.Started = true;
            end
            write(obj.Stream, x);
            s = status(obj.Stream);
            underflow = s.Underflow > 0;
        end

        function processTunedPropertiesImpl(obj)
            args = {};
            if isChangedProperty(obj, 'CenterFrequency')
                args = [args, omnicore.Receiver.optional('TxFrequency', obj.CenterFrequency)];
            end
            if isChangedProperty(obj, 'Gain')
                args = [args, omnicore.Receiver.optional('TxGain', obj.Gain)];
            end
            if ~isempty(args)
                configure(obj.Dev, args{:});
            end
        end

        function releaseImpl(obj)
            if ~isempty(obj.Stream) && isvalid(obj.Stream)
                try
                    stop(obj.Stream);
                catch err
                    warning('omnicore:CloseFailed', 'Stopping the Transmitter: %s', err.message);
                end
                delete(obj.Stream);
            end
            obj.Stream = [];
            if obj.OwnsDevice && ~isempty(obj.Dev) && isvalid(obj.Dev)
                delete(obj.Dev);
            end
            obj.Dev = [];
            obj.Started = false;
        end

        function n = getNumInputsImpl(~)
            n = 1;
        end

        function n = getNumOutputsImpl(~)
            n = 1;
        end

        function s = getOutputSizeImpl(~)
            s = [1 1];
        end

        function t = getOutputDataTypeImpl(~)
            t = 'logical';
        end

        function c = isOutputComplexImpl(~)
            c = false;
        end

        function f = isOutputFixedSizeImpl(~)
            f = true;
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
end
