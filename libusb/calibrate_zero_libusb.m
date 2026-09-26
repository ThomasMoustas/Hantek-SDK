% Measures the zero level of CH1 and CH2 for every gain (libusb path) and saves
% it to hantek_zero_libusb.mat, which tetbench_libusb.m then uses.
%
% Connect both probe tips to their ground clips (0 V at the inputs), then run
% this script from this folder.
clear all

SIMULATE = 0;              % 1 = run without a scope (the levels are then meaningless)
SAMPLE_RATE = 1e6;
N = 200000;                % samples averaged per gain (0.2 s)

LIB = 'HantekUSB';
gains = [1 2 5 10];
zero = zeros(numel(gains), 2);
if ~libisloaded(LIB)
    loadlibrary('HantekUSB.dll', 'HantekUSB.h');
end
try
    Check(calllib(LIB, 'huSetSimulation', SIMULATE));
    Check(calllib(LIB, 'huOpen'));
    Check(calllib(LIB, 'huSetSampleRate', SAMPLE_RATE));
    for g = 1:numel(gains)
        Check(calllib(LIB, 'huSetGain', 0, gains(g)));
        Check(calllib(LIB, 'huSetGain', 1, gains(g)));
        [ret, a, b] = calllib(LIB, 'huReadBlock', zeros(N, 1, 'uint8'), zeros(N, 1, 'uint8'), N);
        Check(ret);
        zero(g, :) = [mean(double(a)), mean(double(b))];
        fprintf('gain %2d: CH1 %.2f, CH2 %.2f counts\n', gains(g), zero(g, 1), zero(g, 2));
    end
    if any(abs(zero(:) - 128) > 20)
        warning('Some levels are far from 128: are both inputs really at 0 V?');
    end
    save('hantek_zero_libusb.mat', 'zero', 'gains');
    fprintf('Saved %s\n', fullfile(pwd, 'hantek_zero_libusb.mat'));
catch ME
    fprintf('\nERROR: %s\n', ME.message);
end
calllib(LIB, 'huClose');
unloadlibrary(LIB);

function ret = Check(ret)
    if ret < 0
        error('HantekUSB error %d: %s', ret, calllib('HantekUSB', 'huLastError'));
    end
end
