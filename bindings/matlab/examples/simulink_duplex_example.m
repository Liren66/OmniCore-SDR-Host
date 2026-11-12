function out = simulink_duplex_example(stopTime)
%SIMULINK_DUPLEX_EXAMPLE Build and run a Simulink model with OmniCore Receiver and Transmitter blocks.
%   OUT = simulink_duplex_example() builds model omnicore_duplex (not saved): a MATLAB System block
%   omnicore.Receiver writing frames to the workspace, and an omnicore.Transmitter block sending a
%   constant tone frame at the same frame rate. Both blocks open the only connected device by
%   SerialNumber; Simulink sets up every block before it starts, so they share the device in full
%   duplex. Runs for STOPTIME seconds of signal (default 2) and returns the simulation output
%   (out.y: received frames, out.overflow, out.underflow). save_system('omnicore_duplex') keeps it.
%   The blocks run in interpreted execution; Simulink runs at the device rate because the
%   Receiver waits for each frame.

if nargin < 1
    stopTime = 2;
end
fs = 30.72e6;
n = 30720;
model = 'omnicore_duplex';
if bdIsLoaded(model)
    close_system(model, 0);
end
new_system(model);
set_param(model, 'SolverType', 'Fixed-step', 'Solver', 'FixedStepDiscrete', 'StopTime', num2str(stopTime), ...
          'SaveOutput', 'off', 'ReturnWorkspaceOutputs', 'on');
mws = get_param(model, 'ModelWorkspace');
mws.assignin('fs', fs);
mws.assignin('n', n);
mws.assignin('txFrame', single(0.5 * exp(1j * 2 * pi * 1e6 / fs * (0:n - 1).')));

add_block('simulink/User-Defined Functions/MATLAB System', [model '/Receiver'], 'System', 'omnicore.Receiver', ...
          'Position', [100 100 250 180]);
set_param([model '/Receiver'], 'SamplesPerFrame', 'n', 'SampleRate', 'fs', 'CenterFrequency', '2.45e9');
add_block('simulink/Sinks/To Workspace', [model '/y'], 'VariableName', 'y', 'SaveFormat', 'Array', ...
          'Position', [350 90 420 110]);
add_block('simulink/Sinks/To Workspace', [model '/overflow'], 'VariableName', 'overflow', ...
          'SaveFormat', 'Array', 'Position', [350 150 420 170]);
add_block('simulink/Sinks/Terminator', [model '/valid'], 'Position', [350 125 370 140]);
add_line(model, 'Receiver/1', 'y/1');
add_line(model, 'Receiver/2', 'valid/1');
add_line(model, 'Receiver/3', 'overflow/1');

add_block('simulink/Sources/Constant', [model '/tone'], 'Value', 'txFrame', 'SampleTime', 'n/fs', ...
          'OutDataTypeStr', 'Inherit: Inherit from ''Constant value''', 'Position', [100 250 170 280]);
add_block('simulink/User-Defined Functions/MATLAB System', [model '/Transmitter'], ...
          'System', 'omnicore.Transmitter', 'Position', [250 240 400 290]);
set_param([model '/Transmitter'], 'CenterFrequency', '2.45e9', 'Gain', '30');
add_block('simulink/Sinks/To Workspace', [model '/underflow'], 'VariableName', 'underflow', ...
          'SaveFormat', 'Array', 'Position', [480 255 550 275]);
add_line(model, 'tone/1', 'Transmitter/1');
add_line(model, 'Transmitter/1', 'underflow/1');

out = sim(model);
end
