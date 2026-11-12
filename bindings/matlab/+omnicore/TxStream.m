classdef TxStream < handle
    %TXSTREAM Streaming writes and timed bursts to an omnicore.Device.
    %   TX = txStream(DEV) or TX = omnicore.TxStream(DEV, Name, Value) with
    %     'BufferBytes'  host ring buffer, default 64 MiB
    %     'Timeout'      seconds write waits without progress, default 1 (settable later)
    %
    %   write(TX, X)                       queue X (one column per TX channel); blocks until all
    %                                      of it is accepted
    %   write(TX, X, 'StartTime', T)       X starts a new burst at hardware time T (seconds)
    %   write(TX, X, 'EndBurst', true)     X ends the burst (X may be empty)
    %   S = status(TX)                     TX events since the last call: Underflow, Starved,
    %                                      Late, BurstAck, AckTime, AckTicks, Errors, LastError, Dropped
    %   stop(TX)                           send what is queued and stop; the next write starts anew
    %
    %   X types as for omnicore.Device.transmit; all writes until stop must use the same kind
    %   (single/double, int16 or int8). The first write after an EndBurst, or with a StartTime,
    %   starts a new burst. The device starts sending once one USB transfer (about 32k samples on
    %   two channels, 64k on one) is queued or a burst has ended; inside a burst, feed it at least
    %   as fast as the sample rate or status reports underflows.
    %
    %   The device has one TX stream: transmit, transmitRepeat and a second stream fail with
    %   omnicore:BUSY while this one is active (from the first write until stop).
    %
    %   Example (timed burst 0.5 s ahead):
    %       dev = omnicore.Device('TxChannels', 1);
    %       tx = txStream(dev);
    %       x = exp(1j*2*pi*1e6/dev.SampleRate*(0:99999).') * 0.5;
    %       write(tx, x, 'StartTime', time(dev) + 0.5, 'EndBurst', true);
    %       pause(1); s = status(tx)
    %
    %   See also omnicore.Device.

    properties (SetAccess = private)
        Device
        BufferBytes = []
    end

    properties
        Timeout = 1
    end

    properties (Dependent, SetAccess = private)
        IsActive
    end

    properties (Access = private)
        Id = uint64(0)
    end

    methods
        function obj = TxStream(dev, varargin)
            if ~(isa(dev, 'omnicore.Device') && isscalar(dev))
                error('omnicore:InvalidArgument', 'TxStream: the first argument must be an omnicore.Device');
            end
            opts = nameValueStruct({'BufferBytes', 'Timeout'}, varargin, 'txStream');
            if isfield(opts, 'Timeout')
                obj.Timeout = opts.Timeout;
                opts = rmfield(opts, 'Timeout');
            end
            obj.Id = omnicore_mex('tx_create', dev.requireId(), opts);
            obj.Device = dev;
            if isfield(opts, 'BufferBytes'), obj.BufferBytes = opts.BufferBytes; end
        end

        function delete(obj)
            if obj.Id ~= 0
                try
                    omnicore_mex('tx_destroy', obj.Id);
                catch err
                    warning('omnicore:CloseFailed', 'Closing the TX stream: %s', err.message);
                end
                obj.Id = uint64(0);
            end
        end

        function write(obj, x, varargin)
            %WRITE Queue samples: write(TX, X, 'StartTime', T, 'EndBurst', TF).
            opts = nameValueStruct({'StartTime', 'EndBurst'}, varargin, 'write');
            omnicore_mex('tx_write', obj.requireId(), x, opts, obj.Timeout);
        end

        function s = status(obj)
            %STATUS TX events since the last call (see the class help).
            s = omnicore_mex('tx_status', obj.requireId());
        end

        function stop(obj)
            %STOP Send what is queued (an open burst is closed with zero padding) and stop.
            omnicore_mex('tx_stop', obj.requireId());
        end

        function tf = get.IsActive(obj)
            tf = false;
            if obj.Id ~= 0
                try
                    st = omnicore_mex('tx_state', obj.Id);
                    tf = st.Active;
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
                error('omnicore:InvalidHandle', 'The TX stream was deleted');
            end
            id = obj.Id;
        end
    end
end
