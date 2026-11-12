classdef Device < handle
    %DEVICE An open OmniCoreSDR (AD9361, 2RX + 2TX, 70 MHz - 6 GHz).
    %   DEV = omnicore.Device() opens the only connected device.
    %   DEV = omnicore.Device(SERIAL) opens the device with that serial number.
    %   DEV = omnicore.Device(..., Name, Value) also sets any configuration property, for
    %   example omnicore.Device('IQBits', 8) or omnicore.Device(SERIAL, 'TxChannels', 1).
    %
    %   Opening applies the OmniCore Host defaults (30.72 MSPS, 1 GHz, RX channels [1 2], no TX)
    %   plus the given values in one step. The device is held exclusively until the object is
    %   deleted (delete(DEV) or clear DEV); other programs, such as the och tool, cannot open it
    %   meanwhile. Opening the same device again in this MATLAB session returns a second object for
    %   the same open device; it is closed when the last such object is deleted.
    %
    %   Configuration properties (settable; each assignment reconfigures the device once):
    %     SampleRate   Hz, 2.5e6 to 61.44e6, shared by RX, TX and both channels. The device
    %                  rounds to its clock chain; read the property back for the applied value.
    %     RxFrequency  Hz, 70e6 to 6e9, one LO for both RX channels.  TxFrequency likewise for TX.
    %     RxBandwidth  Hz, 200e3 to 56e6.  TxBandwidth likewise.
    %     RxGain       dB, 1x2 (one per channel; a scalar sets both), -3 up to 73 / 71 / 62 dB for
    %                  an RX LO up to 1.3 GHz / up to 4 GHz / above. Higher values are lowered to
    %                  the band maximum with warning omnicore:GainLimited.
    %     RxGainMode   'manual', 'fast', 'slow' (default) or 'hybrid'. RxGain applies in 'manual'.
    %     TxGain       dB, 1x2, 0 to 89.75 in 0.25 dB steps; TxGain = 89.75 - attenuation.
    %     RxChannels   [1], [2], [1 2] or [] (receiver off).  TxChannels likewise.
    %                  Change only while no stream exists.
    %     IQBits       12 (default) or 8. 8 doubles the USB sample rate headroom and needs
    %                  RxChannels = TxChannels = [1 2].
    %   Use configure(DEV, Name, Value, ...) to change several at once with a single
    %   reconfiguration (a sample-rate and bandwidth change then pauses streams only once).
    %
    %   Board settings: RxLOSource, TxLOSource ('internal' / 'external'), PpsEnable, RssiEnable,
    %   RgbEnable, FanEnable, RxDCCorrection, RxIQCorrection (logical).
    %
    %   Read only: Serial, TxAttenuation.
    %
    %   Methods: configure, info, status, receive, rxStream, delete.
    %
    %   Receiving:
    %       [x, meta] = receive(DEV, N) captures N samples per enabled RX channel: an N x numel(
    %       RxChannels) complex single array scaled to +-1. meta.Time is the hardware time of the
    %       first sample and meta.Gaps lists lost samples; see help omnicore.Device.receive.
    %       For continuous reception use RX = rxStream(DEV); see omnicore.RxStream.
    %
    %   Errors have identifiers omnicore:<reason>, for example omnicore:BUSY when the device is in
    %   use by another program, omnicore:NOT_FOUND, omnicore:InvalidArgument.
    %
    %   Example:
    %       dev = omnicore.Device();
    %       configure(dev, 'SampleRate', 30.72e6, 'RxFrequency', 2.45e9, 'RxBandwidth', 20e6);
    %       dev.RxGainMode = 'manual';
    %       dev.RxGain = [40 40];
    %       status(dev)
    %
    %   See also omnicore.findDevices.

    properties (Dependent)
        SampleRate
        RxFrequency
        TxFrequency
        RxBandwidth
        TxBandwidth
        RxGain
        RxGainMode
        TxGain
        RxChannels
        TxChannels
        IQBits

        RxLOSource
        TxLOSource
        PpsEnable
        RssiEnable
        RgbEnable
        FanEnable
        RxDCCorrection
        RxIQCorrection
    end

    properties (Dependent, SetAccess = private)
        Serial
        TxAttenuation
    end

    properties (Access = private)
        Id = uint64(0)
        CachedSerial = ''
    end

    properties (Constant, Hidden)
        ConfigFields = {'SampleRate', 'RxFrequency', 'TxFrequency', 'RxBandwidth', 'TxBandwidth', 'RxGain', ...
                        'RxGainMode', 'TxGain', 'RxChannels', 'TxChannels', 'IQBits'}
        TxAttenuationMax = 89.75
    end

    methods
        function obj = Device(varargin)
            serial = '';
            args = varargin;
            if mod(numel(args), 2) == 1
                serial = args{1};
                if ~(ischar(serial) || (isstring(serial) && isscalar(serial)))
                    error('omnicore:InvalidArgument', 'Device: the serial number must be text');
                end
                serial = char(serial);
                args = args(2:end);
            end
            request = nameValueStruct(omnicore.Device.ConfigFields, args, 'Device');
            obj.Id = omnicore_mex('open', serial, request);
            obj.CachedSerial = obj.info().Serial;
        end

        function delete(obj)
            if obj.Id ~= 0
                try
                    omnicore_mex('close', obj.Id);
                catch err
                    warning('omnicore:CloseFailed', 'Closing %s: %s', obj.CachedSerial, err.message);
                end
                obj.Id = uint64(0);
            end
        end

        function configure(obj, varargin)
            %CONFIGURE Set several configuration properties with one reconfiguration.
            %   configure(DEV, 'SampleRate', 61.44e6, 'RxBandwidth', 56e6, 'TxBandwidth', 56e6)
            request = nameValueStruct(omnicore.Device.ConfigFields, varargin, 'configure');
            omnicore_mex('configure', obj.requireId(), request);
        end

        function s = info(obj)
            %INFO Identification: Serial, Name, Firmware, UsbDriver, UsbSpeedMbps, channel counts,
            %   HardwareTimestamps, MaxSampleRate and LibraryVersion.
            s = omnicore_mex('info', obj.requireId());
        end

        function s = status(obj)
            %STATUS Live status: Temperature (degC, NaN if unavailable), RefLocked, PpsLocked,
            %   VcxoLocked, TimeValid, UsbSpeedMbps and RxRssi (relative dB per channel, NaN while
            %   not receiving or RssiEnable is off). Locks read false when unknown.
            s = omnicore_mex('status', obj.requireId());
        end

        function [x, meta] = receive(obj, n, varargin)
            %RECEIVE Capture N samples per enabled RX channel in one finite burst.
            %   [X, META] = receive(DEV, N) returns X, an N x numel(DEV.RxChannels) complex array (one
            %   column per channel), and META, a struct:
            %     Time          hardware time of the first sample, seconds (NaN if unknown)
            %     TimeTicks     the same in samples (uint64), without rounding
            %     Overflow      true if samples were lost (X then has fewer than N rows)
            %     Gaps          K x 2, [row of the first sample after a loss, samples lost];
            %                   the count is NaN when unknown
            %     Late          a timed start was already past
            %     Reconfigured  the device was reconfigured during the capture
            %   [X, META] = receive(DEV, N, Name, Value) with
            %     'OutputDataType'  'single' (default, +-1), 'double', 'int16' (12-bit samples
            %                       left-aligned, +-32768) or 'int8' (IQBits 8 only)
            %     'Timeout'         seconds to wait for each block, default 1
            %     'BufferBytes'     host ring buffer, default 64 MiB
            %   Memory: N x channels x 8 bytes for single, 16 for double, 4 for int16, 2 for int8;
            %   1e8 samples on two channels in single take 1.6 GB.
            %
            %   Example:
            %       dev.SampleRate = 30.72e6;
            %       [x, meta] = receive(dev, 1e6);
            %       pwelch(x(:, 1), [], [], [], dev.SampleRate, 'centered')
            if ~(isnumeric(n) && isscalar(n))
                error('omnicore:InvalidArgument', 'receive: N must be a numeric scalar');
            end
            opts = nameValueStruct({'OutputDataType', 'Timeout', 'BufferBytes'}, varargin, 'receive');
            if nargout > 1
                [x, meta] = omnicore_mex('receive', obj.requireId(), double(n), opts);
            else
                x = omnicore_mex('receive', obj.requireId(), double(n), opts);
            end
        end

        function rx = rxStream(obj, varargin)
            %RXSTREAM A continuous RX stream on this device; see omnicore.RxStream.
            %   RX = rxStream(DEV, 'OutputDataType', 'int16', 'BufferBytes', 256*2^20, 'Timeout', 2)
            rx = omnicore.RxStream(obj, varargin{:});
        end

        function tf = isOpen(obj)
            %ISOPEN True until the object is deleted.
            tf = isvalid(obj) && obj.Id ~= 0;
        end

        function disp(obj)
            if ~isscalar(obj)
                builtin('disp', obj);
                return
            end
            if ~obj.isOpen()
                fprintf('  omnicore.Device (closed)\n\n');
                return
            end
            c = omnicore_mex('config', obj.Id);
            fprintf('  omnicore.Device %s\n\n', obj.CachedSerial);
            fprintf('      SampleRate: %.6g Hz\n', c.SampleRate);
            fprintf('     RxFrequency: %.6g Hz      TxFrequency: %.6g Hz\n', c.RxFrequency, c.TxFrequency);
            fprintf('     RxBandwidth: %.6g Hz      TxBandwidth: %.6g Hz\n', c.RxBandwidth, c.TxBandwidth);
            fprintf('          RxGain: [%s] dB (%s)\n', num2str(c.RxGain), c.RxGainMode);
            fprintf('          TxGain: [%s] dB\n', num2str(c.TxGain));
            fprintf('      RxChannels: [%s]     TxChannels: [%s]     IQBits: %d\n\n', ...
                    num2str(c.RxChannels), num2str(c.TxChannels), c.IQBits);
        end

        % --- configuration properties ---------------------------------------------------------
        function v = get.SampleRate(obj),  v = obj.config('SampleRate');  end
        function v = get.RxFrequency(obj), v = obj.config('RxFrequency'); end
        function v = get.TxFrequency(obj), v = obj.config('TxFrequency'); end
        function v = get.RxBandwidth(obj), v = obj.config('RxBandwidth'); end
        function v = get.TxBandwidth(obj), v = obj.config('TxBandwidth'); end
        function v = get.RxGain(obj),      v = obj.config('RxGain');      end
        function v = get.RxGainMode(obj),  v = obj.config('RxGainMode');  end
        function v = get.TxGain(obj),      v = obj.config('TxGain');      end
        function v = get.RxChannels(obj),  v = obj.config('RxChannels');  end
        function v = get.TxChannels(obj),  v = obj.config('TxChannels');  end
        function v = get.IQBits(obj),      v = obj.config('IQBits');      end
        function v = get.TxAttenuation(obj), v = omnicore.Device.TxAttenuationMax - obj.config('TxGain'); end
        function v = get.Serial(obj),      v = obj.CachedSerial;          end

        function set.SampleRate(obj, v),  obj.configure('SampleRate', v);  end
        function set.RxFrequency(obj, v), obj.configure('RxFrequency', v); end
        function set.TxFrequency(obj, v), obj.configure('TxFrequency', v); end
        function set.RxBandwidth(obj, v), obj.configure('RxBandwidth', v); end
        function set.TxBandwidth(obj, v), obj.configure('TxBandwidth', v); end
        function set.RxGain(obj, v),      obj.configure('RxGain', v);      end
        function set.RxGainMode(obj, v),  obj.configure('RxGainMode', v);  end
        function set.TxGain(obj, v),      obj.configure('TxGain', v);      end
        function set.RxChannels(obj, v),  obj.configure('RxChannels', v);  end
        function set.TxChannels(obj, v),  obj.configure('TxChannels', v);  end
        function set.IQBits(obj, v),      obj.configure('IQBits', v);      end

        % --- board settings -------------------------------------------------------------------
        function v = get.RxLOSource(obj),     v = obj.getSetting('RxLOSource');     end
        function v = get.TxLOSource(obj),     v = obj.getSetting('TxLOSource');     end
        function v = get.PpsEnable(obj),      v = obj.getSetting('PpsEnable');      end
        function v = get.RssiEnable(obj),     v = obj.getSetting('RssiEnable');     end
        function v = get.RgbEnable(obj),      v = obj.getSetting('RgbEnable');      end
        function v = get.FanEnable(obj),      v = obj.getSetting('FanEnable');      end
        function v = get.RxDCCorrection(obj), v = obj.getSetting('RxDCCorrection'); end
        function v = get.RxIQCorrection(obj), v = obj.getSetting('RxIQCorrection'); end

        function set.RxLOSource(obj, v),     obj.setSetting('RxLOSource', v);     end
        function set.TxLOSource(obj, v),     obj.setSetting('TxLOSource', v);     end
        function set.PpsEnable(obj, v),      obj.setSetting('PpsEnable', v);      end
        function set.RssiEnable(obj, v),     obj.setSetting('RssiEnable', v);     end
        function set.RgbEnable(obj, v),      obj.setSetting('RgbEnable', v);      end
        function set.FanEnable(obj, v),      obj.setSetting('FanEnable', v);      end
        function set.RxDCCorrection(obj, v), obj.setSetting('RxDCCorrection', v); end
        function set.RxIQCorrection(obj, v), obj.setSetting('RxIQCorrection', v); end
    end

    methods (Access = {?omnicore.RxStream})
        function id = requireId(obj)
            if obj.Id == 0
                error('omnicore:InvalidHandle', 'The OmniCore device was closed; create a new omnicore.Device');
            end
            id = obj.Id;
        end
    end

    methods (Access = private)
        function v = config(obj, field)
            c = omnicore_mex('config', obj.requireId());
            v = c.(field);
        end

        function v = getSetting(obj, name)
            v = omnicore_mex('get', obj.requireId(), name);
        end

        function setSetting(obj, name, v)
            if isstring(v)
                v = char(v);
            elseif islogical(v) || (isnumeric(v) && ~isa(v, 'double'))
                v = double(v);
            end
            omnicore_mex('set', obj.requireId(), name, v);
        end
    end
end
