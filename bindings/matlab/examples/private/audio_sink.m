function sink = audio_sink(fs, enable)
%AUDIO_SINK Play audio blocks as they come: SINK.push(X), SINK.close().
%   With Audio Toolbox or DSP System Toolbox, audioDeviceWriter plays the blocks seamlessly.
%   Base MATLAB has only audioplayer, which plays a whole buffer at a time: blocks are collected
%   into 0.5 s chunks and the next chunk starts when the previous one has ended, so short gaps
%   are audible. ENABLE false makes push and close do nothing.

sink.push = @(x) [];
sink.close = @() [];
sink.kind = 'none';
if ~enable
    return
end
fs = round(fs);
writer = [];
try
    % feval keeps the toolbox dependency optional (requiredFilesAndProducts does not list it).
    writer = feval('audioDeviceWriter', 'SampleRate', fs);
catch
end
if ~isempty(writer)
    sink.push = @(x) writeBlock(x);
    sink.close = @() release(writer);
    sink.kind = 'audioDeviceWriter';
    return
end

pending = zeros(0, 1);
player = [];
sink.push = @pushChunk;
sink.close = @stopPlayer;
sink.kind = 'audioplayer';

    function writeBlock(x)
        writer(single(min(max(x(:), -1), 1)));
    end

    function pushChunk(x)
        pending = [pending; min(max(x(:), -1), 1)];
        if numel(pending) >= fs / 2 && (isempty(player) || ~isplaying(player))
            player = audioplayer(pending, fs);
            play(player);
            pending = zeros(0, 1);
        elseif numel(pending) > 4 * fs
            pending = pending(end - fs + 1:end);    % playing behind: drop the oldest audio
        end
    end

    function stopPlayer()
        if ~isempty(player)
            stop(player);
        end
    end
end
