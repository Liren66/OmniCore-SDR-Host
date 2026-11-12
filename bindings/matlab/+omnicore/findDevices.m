function devices = findDevices()
%FINDDEVICES List connected OmniCoreSDR devices.
%   DEVICES = omnicore.findDevices() returns an Nx1 struct array, sorted by serial number, with
%   fields Serial, Name, Firmware, UsbDriver, UsbSpeedMbps, RxChannelCount and TxChannelCount.
%   Devices already open (in this or another program) are listed too.
%
%   See also omnicore.Device.

devices = omnicore_mex('list');
end
