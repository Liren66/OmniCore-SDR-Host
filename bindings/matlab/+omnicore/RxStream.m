classdef RxStream < handle
    %RXSTREAM Continuous reception from an omnicore.Device.
    %   RX = rxStream(DEV) or RX = omnicore.RxStream(DEV, Name, Value) with
    %     'OutputDataType'  'single' (default, +-1), 'double', 'int16' or 'int8' (IQBits 8 only)
    %     'BufferBytes'     host ring buffer, default 64 MiB; samples arriving while MATLAB is not
    %                       reading wait here, in the wire format (64 MiB holds about 0.36 s
    %                       of two channels at 30.72 MSPS with 12-bit samples)
    %     'Timeout'         seconds read waits for each block, default 1 (settable later)
    %
    %   start(RX)                 starts streaming; the device keeps receiving between reads
    %   start(RX, 'StartTime', T) starts at hardware time T (seconds)
    %   [X, META] = read(RX, N)   N samples per channel, N x numel(DEV.RxChannels) complex
    %   stop(RX)                  stops; start again for a new stream
    %
    %   META is as for omnicore.Device.receive. Samples lost while MATLAB was busy (the ring
    %   buffer overflowed) show as META.Overflow and a row of META.Gaps, at row 1 if they were lost
    %   before this read. read returns fewer than N rows when a block takes longer than Timeout
    %   after some samples arrived, and errors with omnicore:TIMEOUT when none arrived.
    %
    %   The device has one RX stream: receive() and a second stream fail with omnicore:BUSY while
    %   this one runs. Channel and IQBits changes need the stream stopped; frequency and gain
    %   changes apply while it runs (META.Reconfigured), a sample-rate change pauses it.
    %
    %   Example:
    %       rx = rxStream(dev);
    %       start(rx);
    %       for k = 1:100
    %           [x, meta] = read(rx, 65536);
    %           if meta.Overflow, fprintf('lost %g samples\n', sum(meta.Gaps(:, 2))); end
    %       end
    %       stop(rx);
    %
    %   See also omnicore.Device.

    properties (SetAccess = private)
        Device
        OutputDataType = 'single'
        BufferBytes = []
    end

    properties
        Timeout = 1
    end

    properties (Dependent, SetAccess = private)
        IsRunning
    end

    properties (Access = private)
        Id = uint64(0)
    end

    methods
        function obj = RxStream(dev, varargin)
            if ~(isa(dev, 'omnicore.Device') && isscalar(dev))
                error('omnicore:InvalidArgument', 'RxStream: the first argument must be an omnicore.Device');
            end
            opts = nameValueStruct({'OutputDataType', 'BufferBytes', 'Timeout'}, varargin, 'rxStream');
            if isfield(opts, 'Timeout')
                obj.Timeout = opts.Timeout;
                opts = rmfield(opts, 'Timeout');
            end
            obj.Id = omnicore_mex('rx_create', dev.requireId(), opts);
            obj.Device = dev;
            if isfield(opts, 'OutputDataType'), obj.OutputDataType = opts.OutputDataType; end
            if isfield(opts, 'BufferBytes'), obj.BufferBytes = opts.BufferBytes; end
        end

        function delete(obj)
            if obj.Id ~= 0
                try
                    omnicore_mex('rx_destroy', obj.Id);
                catch err
                    warning('omnicore:CloseFailed', 'Closing the RX stream: %s', err.message);
                end
                obj.Id = uint64(0);
            end
        end

        function start(obj, varargin)
            %START Start streaming. Creates the device stream; errors if it is already running.
            %   start(RX, 'StartTime', T) starts at hardware time T (seconds, see
            %   omnicore.Device.time); read waits Timeout for each block, so make it longer than the
            %   delay to T for the first read.
            opts = nameValueStruct({'StartTime'}, varargin, 'start');
            t = [];
            if isfield(opts, 'StartTime')
                t = opts.StartTime;
            end
            omnicore_mex('rx_start', obj.requireId(), t);
        end

        function stop(obj)
            %STOP Stop streaming. Samples not yet read are discarded.
            omnicore_mex('rx_stop', obj.requireId());
        end

        function [x, meta] = read(obj, n)
            %READ Read N samples per channel: [X, META] = read(RX, N).
            if ~(isnumeric(n) && isscalar(n))
                error('omnicore:InvalidArgument', 'read: N must be a numeric scalar');
            end
            if nargout > 1
                [x, meta] = omnicore_mex('rx_read', obj.requireId(), double(n), obj.Timeout);
            else
                x = omnicore_mex('rx_read', obj.requireId(), double(n), obj.Timeout);
            end
        end

        function tf = get.IsRunning(obj)
            tf = false;
            if obj.Id ~= 0
                try
                    tf = omnicore_mex('rx_running', obj.Id);
                catch
                    tf = false; % the device was closed
                end
            end
        end

        function set.Timeout(obj, v)
            if ~(isnumeric(v) && isscalar(v) && isreal(v) && v > 0 && v <= 3600)
                error('omnicore:InvalidArgument', 'Timeout must be more than 0 and at most 3600 seconds');
            end
            obj.Timeout = double(v);
        end
    end

    methods (Access = private)
        function id = requireId(obj)
            if obj.Id == 0
                error('omnicore:InvalidHandle', 'The RX stream was deleted');
            end
            id = obj.Id;
        end
    end
end
