function v = version()
%VERSION OmniCore Host versions used by the MATLAB interface.
%   V = omnicore.version() returns a struct: Library (the loaded OmniCore Host library), Header
%   (the och.h the MEX file was built with) and Abi (its C ABI version).

v = omnicore_mex('version');
end
