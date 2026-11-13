% OmniCoreSDR for MATLAB
%
% Devices
%   Device       - An open OmniCoreSDR: configuration, receive, transmit, time
%   findDevices  - List connected devices
%   version      - OmniCore Host library, header and ABI versions
%
% Streams
%   RxStream     - Continuous reception (rxStream(dev))
%   TxStream     - Streaming writes and timed bursts (txStream(dev))
%
% System objects (also MATLAB System blocks in Simulink)
%   Receiver     - Frame-based reception
%   Transmitter  - Frame-based transmission
%
% Documentation
%   doc          - Open the reference pages: omnicore.doc, omnicore.doc('receive')
%
% Examples (in the examples folder next to +omnicore):
%   fm_radio, fm_loopback, video_link, sdr_scope, rx_spectrum, tx_tone_repeat,
%   duplex_system_objects, timed_burst_loopback, simulink_duplex_example
