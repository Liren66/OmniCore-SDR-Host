function s = nameValueStruct(allowed, args, context)
%NAMEVALUESTRUCT Name-value pairs -> struct for omnicore_mex, with names checked against ALLOWED.
%   Names match case-insensitively and are stored with the spelling in ALLOWED. Strings become
%   char vectors and logicals become double, the only types the MEX gateway accepts.

if mod(numel(args), 2) ~= 0
    error('omnicore:InvalidArgument', '%s: expected name-value pairs', context);
end
s = struct();
for k = 1:2:numel(args)
    name = args{k};
    if ~(ischar(name) || (isstring(name) && isscalar(name)))
        error('omnicore:InvalidArgument', '%s: parameter names must be text', context);
    end
    match = allowed(strcmpi(allowed, char(name)));
    if isempty(match)
        error('omnicore:InvalidArgument', '%s: unknown parameter ''%s''. Valid: %s', context, char(name), ...
              strjoin(allowed, ', '));
    end
    s.(match{1}) = toMexValue(args{k + 1});
end
end

function v = toMexValue(v)
if isstring(v)
    v = char(v);
elseif islogical(v) || (isnumeric(v) && ~isa(v, 'double'))
    v = double(v);
end
end
