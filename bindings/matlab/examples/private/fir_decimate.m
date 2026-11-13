function [y, st] = fir_decimate(x, st)
%FIR_DECIMATE Run a polyphase decimator made by fir_decimator on the next block X (a column).
%   Blocks of any length work; samples left over after the last whole group of D wait for the
%   next call.

x = [st.pending; x(:)];
m = floor(numel(x) / st.d);
st.pending = x(m * st.d + 1:end);
if m == 0
    y = zeros(0, 1, 'like', x);
    return
end
rows = reshape(x(1:m * st.d), st.d, m);
y = zeros(m, 1, 'like', x);
if ~isreal(x) && isreal(st.zi)
    st.zi = complex(st.zi);
end
for r = 1:st.d
    [yr, st.zi(:, r)] = filter(st.taps{r}, 1, rows(r, :).', st.zi(:, r));
    y = y + yr;
end
end
