function st = fir_decimator(h, d)
%FIR_DECIMATOR State of a polyphase FIR decimator: filter with H, keep every D-th sample.
%   ST = fir_decimator(H, D), then repeatedly [Y, ST] = fir_decimate(X, ST). The result equals
%   y = filter(H, 1, x); y = y(1:D:end) over the concatenated input, at 1/D of the cost: each of
%   the D polyphase branches filters only the input samples that reach it.

h = h(:);
k = ceil(numel(h) / d);
h(end + 1:k * d) = 0;
% Output m sums h(n) x(mD - n). With n = kD + p, branch p sees x(jD - p): for p = 0 that is
% row 1 of reshape(x, D, []) at column j, for p >= 1 row D - p + 1 one column earlier, which
% the extra leading zero tap provides.
st.d = d;
st.taps = cell(d, 1);
st.taps{1} = [h(1:d:end); 0];
for p = 1:d - 1
    st.taps{d - p + 1} = [0; h(p + 1:d:end)];
end
st.zi = zeros(k, d);
st.pending = zeros(0, 1);   % input samples not yet a whole multiple of D
end
