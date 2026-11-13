function opt = parse_options(opt, args)
%PARSE_OPTIONS Name-value pairs ARGS over the defaults in struct OPT (names case-insensitive).

if mod(numel(args), 2) ~= 0
    error('omnicore:InvalidArgument', 'Options must be name-value pairs');
end
names = fieldnames(opt);
for k = 1:2:numel(args)
    i = find(strcmpi(names, args{k}), 1);
    if isempty(i)
        error('omnicore:InvalidArgument', 'Unknown option ''%s''; known: %s', char(args{k}), ...
              strjoin(names.', ', '));
    end
    opt.(names{i}) = args{k + 1};
end
end
