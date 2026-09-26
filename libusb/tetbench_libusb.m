% Hantek 6022BL logger - libusb path (native 64-bit, no HantekProxy.exe / HTMarch.dll)
%
% HantekUSB.dll talks to the scope directly through libusb and the open-source
% sigrok fx2lafw firmware, and records continuously: consecutive samples are
% exactly 1/SAMPLE_RATE apart, with no gaps between reads.
%
% One-time setup on Windows (README.md in this folder): the scope must use the
% WinUSB driver instead of the Hantek driver (Zadig). While WinUSB is
% installed, the Hantek software and the SDK path (tetbench_sdk.m) do not work;
% README.md explains how to switch back.
%
% Run from this folder: HantekUSB.dll, HantekUSB.h and the two
% fx2lafw-hantek-*.fw files must be here. SIMULATE = 1 runs without a scope.
clear all
close all

SIMULATE = 0;              % 0 = real scope, 1 = simulated signals (no hardware needed)
SAMPLE_RATE = 1e6;         % Sa/s: 48e6 30e6 24e6 16e6 12e6 8e6 4e6 2e6 1e6 500e3 200e3 100e3
                           % Both channels always stream: 2*SAMPLE_RATE bytes/s over USB.
                           % Up to about 8e6 is realistic; the measured rate printed at
                           % the end shows whether USB kept up.
GAIN = [1 1];              % CH1, CH2 hardware gain 1, 2, 5 or 10: range about +-5.12 V / gain
DURATION = 5;              % seconds to record
MODE = 'stream';           % 'stream' = continuous recording, 'block' = one huReadBlock call

LIB = 'HantekUSB';
if ~libisloaded(LIB)
    [notfound, warnings] = loadlibrary('HantekUSB.dll', 'HantekUSB.h');
    if ~isempty(notfound)
        error('Missing functions in HantekUSB.dll: %s', strjoin(notfound, ', '));
    end
end

try
    Check(calllib(LIB, 'huSetSimulation', SIMULATE));
    model = Check(calllib(LIB, 'huOpen'));
    models = {'6022BL', '6022BE'};
    fprintf('Opened a Hantek %s%s\n', models{model}, repmat(' (simulated)', 1, SIMULATE > 0));
    Check(calllib(LIB, 'huSetSampleRate', SAMPLE_RATE));
    for ch = 1:2
        Check(calllib(LIB, 'huSetGain', ch - 1, GAIN(ch)));
    end

    n = round(DURATION * SAMPLE_RATE);
    if strcmp(MODE, 'block')
        [ret, raw1, raw2] = calllib(LIB, 'huReadBlock', zeros(n, 1, 'uint8'), zeros(n, 1, 'uint8'), n);
        Check(ret);
        idx = (0:n-1)';
    else
        [raw1, raw2, idx] = StreamRecord(LIB, n, SAMPLE_RATE);
    end
    [~, st] = calllib(LIB, 'getQueueStatus', zeros(1, 12));
    ReportStatus(st);

    zero = ZeroLevels(GAIN, SAMPLE_RATE);
    data.time = idx / SAMPLE_RATE;                      % s, exact (from the stream index)
    data.ch1 = (double(raw1) - zero(1)) / (25 * GAIN(1));  % V (nominal 25 counts/V at gain 1)
    data.ch2 = (double(raw2) - zero(2)) / (25 * GAIN(2));
    data.clipped = [sum(raw1 == 0 | raw1 == 255), sum(raw2 == 0 | raw2 == 255)];
    PlotData(data);
catch ME
    fprintf('\nERROR: %s\n', ME.message);
    for st = ME.stack'
        fprintf('  In %s (line %d)\n', st.name, st.line);
    end
end

calllib(LIB, 'huClose');   % always close before unloading (stops the USB thread)
unloadlibrary(LIB);

%% Continuous recording of n samples per channel
function [ch1, ch2, idx] = StreamRecord(LIB, n, rate)
    ch1 = zeros(n, 1, 'uint8');
    ch2 = zeros(n, 1, 'uint8');
    idx = zeros(n, 1);
    chunk = max(round(rate * 0.25), 65536);     % read at least every ~0.25 s of data
    got = 0;
    Check(calllib(LIB, 'startStreaming', 0));   % ring buffer of ~10 s
    try
        t0 = tic;
        while got < n
            [k, c1, c2, first] = calllib(LIB, 'getStreamData', ...
                zeros(chunk, 1, 'uint8'), zeros(chunk, 1, 'uint8'), chunk, 0);
            Check(k);
            if k == 0
                if toc(t0) > n / rate + 10
                    error('No data after %.0f s', toc(t0));
                end
                pause(0.02);
                continue
            end
            k = min(k, n - got);
            ch1(got+1:got+k) = c1(1:k);
            ch2(got+1:got+k) = c2(1:k);
            idx(got+1:got+k) = first + (0:k-1)';
            got = got + k;
        end
    catch ME
        calllib(LIB, 'stopStreaming');
        rethrow(ME);
    end
    Check(calllib(LIB, 'stopStreaming'));
end

%% Zero level (raw counts) for CH1, CH2
% Order of preference: hantek_zero_libusb.mat (calibrate_zero_libusb.m, measured
% with this firmware), hantek_cal.mat (factory levels saved by ../tetbench_sdk.m),
% the nominal 128.
function zero = ZeroLevels(gain, rate)
    gains = [1 2 5 10];
    zero = [128 128];
    if isfile('hantek_zero_libusb.mat')
        s = load('hantek_zero_libusb.mat');         % zero(gainIndex, channel)
        for ch = 1:2
            zero(ch) = s.zero(gains == gain(ch), ch);
        end
        fprintf('Zero levels from hantek_zero_libusb.mat: %.2f, %.2f\n', zero);
    elseif isfile(fullfile('..', 'hantek_cal.mat'))
        s = load(fullfile('..', 'hantek_cal.mat'));
        voltDiv = [5 4 3 2];                        % nVoltDIV of the SDK for gain 1, 2, 5, 10
        hs = rate > 16e6;                           % the SDK keeps levels for 48 MSa/s apart
        for ch = 1:2
            z = s.cal(16 * hs + 2 * voltDiv(gains == gain(ch)) + ch);
            if z >= 96 && z <= 160
                zero(ch) = z;
            end
        end
        fprintf('Zero levels from the factory calibration (../hantek_cal.mat): %g, %g\n', zero);
    else
        fprintf('No calibration file: nominal zero level 128 (run calibrate_zero_libusb.m)\n');
    end
end

function ReportStatus(st)
    fprintf('\nAcquisition: %.2f s, %d samples delivered, measured rate %.0f Sa/s (set %.0f)\n', ...
        st(7), st(4), st(9), st(8));
    if st(5) > 0
        warning('%d samples dropped in %d gap(s): MATLAB did not read fast enough.', st(5), st(6));
    end
    if st(7) > 2 && st(9) < 0.99 * st(8)
        warning(['The scope delivered only %.1f%% of the samples: USB could not keep up ' ...
                 'and data was lost. Use a lower SAMPLE_RATE.'], 100 * st(9) / st(8));
    end
end

function PlotData(data)
    % Break the lines where the stream index jumps (gaps), so they stay visible
    gap = [false; diff(data.time) > 1.5 * median(diff(data.time))];
    t = data.time;
    y1 = data.ch1;
    y2 = data.ch2;
    y1(gap) = NaN;
    y2(gap) = NaN;

    figure('Name', 'Hantek 6022BL Data (libusb)', 'NumberTitle', 'off');
    subplot(2,1,1);
    plot(t, y1);
    title('Channel 1');
    xlabel('Time (s)');
    ylabel('Voltage (V)');
    grid on;

    subplot(2,1,2);
    plot(t, y2);
    title('Channel 2');
    xlabel('Time (s)');
    ylabel('Voltage (V)');
    grid on;

    fprintf('\nCapture Statistics:\n');
    fprintf('  Samples: %d over %.3f s, gaps: %d\n', numel(t), t(end) - t(1), sum(gap));
    fprintf('  CH1 Range: [%.3f, %.3f] V\n', min(data.ch1), max(data.ch1));
    fprintf('  CH2 Range: [%.3f, %.3f] V\n', min(data.ch2), max(data.ch2));
    if any(data.clipped)
        warning('Clipping: %d (CH1) / %d (CH2) samples at the ADC limits. Use a lower GAIN.', ...
            data.clipped(1), data.clipped(2));
    end
end

function ret = Check(ret)
    % HantekUSB returns a negative code on failure; huLastError explains it.
    if ret < 0
        error('HantekUSB error %d: %s', ret, calllib('HantekUSB', 'huLastError'));
    end
end
