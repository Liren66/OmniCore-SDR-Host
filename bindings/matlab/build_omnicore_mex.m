function build_omnicore_mex(prefix, varargin)
%BUILD_OMNICORE_MEX Build the OmniCore MEX gateway with MATLAB's mex command (no CMake needed).
%   BUILD_OMNICORE_MEX(PREFIX) compiles omnicore_mex against an installed OmniCore Host under
%   PREFIX (the folder that holds include/omnicore/och.h) and stages a ready-to-use folder:
%   the MEX file, the +omnicore package and the OmniCore Host runtime library.
%
%   BUILD_OMNICORE_MEX(PREFIX, 'OutputDir', DIR) stages into DIR (default: ./omnicore-matlab).
%   BUILD_OMNICORE_MEX(PREFIX, 'LibDir', DIR) looks for the library in DIR (default: PREFIX/lib,
%   on Windows PREFIX/bin for omnicore.dll and PREFIX/lib for the MSVC import library omnicore.lib,
%   see packaging/windows/build-msvc-import.ps1).
%
%   Then: addpath(DIR); omnicore.findDevices()
%
%   Example:
%       build_omnicore_mex('/opt/omnicore/0.7.2')

p = inputParser;
p.addRequired('prefix', @(s) ischar(s) || isstring(s));
p.addParameter('OutputDir', fullfile(pwd, 'omnicore-matlab'), @(s) ischar(s) || isstring(s));
p.addParameter('LibDir', '', @(s) ischar(s) || isstring(s));
p.parse(prefix, varargin{:});
prefix = char(p.Results.prefix);
out = char(p.Results.OutputDir);
libdir = char(p.Results.LibDir);
if isempty(libdir)
    libdir = fullfile(prefix, 'lib');
end

here = fileparts(mfilename('fullpath'));
header = fullfile(prefix, 'include', 'omnicore', 'och.h');
if ~isfile(header)
    error('omnicore:build', 'No OmniCore Host headers under %s (missing %s)', prefix, header);
end
if ~isfolder(out)
    mkdir(out);
end

sources = fullfile(here, 'mex', {'omnicore_mex.cpp', 'Registry.cpp', 'DeviceSession.cpp', 'LibraryLoader.cpp'});
args = [{'-R2018a', '-outdir', out, ...
         ['-I' fullfile(prefix, 'include')], ['-I' fullfile(here, '..')], ['-I' fullfile(here, 'mex')]}, sources];

if ispc
    runtime = {fullfile(prefix, 'bin', 'omnicore.dll'), fullfile(prefix, 'bin', 'libusbK.dll')};
    args = [args, {fullfile(libdir, 'omnicore.lib'), 'LINKFLAGS=$LINKFLAGS /DELAYLOAD:omnicore.dll delayimp.lib', ...
                   'COMPFLAGS=$COMPFLAGS /std:c++17'}];
elseif ismac
    runtime = {fullfile(libdir, 'libomnicore.1.dylib')};
    args = [args, {['-L' libdir], '-lomnicore', 'CXXFLAGS=$CXXFLAGS -std=c++17', ...
                   'LDFLAGS=$LDFLAGS -Wl,-rpath,@loader_path'}];
else
    runtime = {fullfile(libdir, 'libomnicore.so.1')};
    args = [args, {['-L' libdir], '-lomnicore', 'CXXFLAGS=$CXXFLAGS -std=c++17', ...
                   'LDFLAGS=$LDFLAGS -Wl,-rpath,''$ORIGIN'''}];
end

mex(args{:});

for k = 1:numel(runtime)
    if isfile(runtime{k})
        copyfile(runtime{k}, out);
    else
        warning('omnicore:build', 'Runtime library %s not found; put it next to the MEX file or on the library path', runtime{k});
    end
end
copyfile(fullfile(here, '+omnicore'), fullfile(out, '+omnicore'));
fprintf('OmniCore MEX gateway staged in %s\n  addpath(''%s'')\n', out, out);
end
