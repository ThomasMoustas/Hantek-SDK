% Hantek 6022BL MATLAB Interface - SDK path (HantekWrapper.dll -> HantekProxy.exe -> HTMarch.dll)
%
% Plain-script version of tetbench.mlx (tetbench.mlx itself is left unchanged).
% Right-click -> "Open as Live Script" if you prefer the live editor.
%
% Needs in the Current Folder: HantekWrapper.dll, HantekWrapper.h,
% HantekProxy.exe and HTMarch.dll, the Hantek driver installed and the
% Hantek 6022BL connected. The script starts and stops HantekProxy.exe itself.
%
% Every capture is a separate acquisition: HTMarch.dll makes the scope record
% 1048576 samples per call (whatever bufferSize is) and returns bufferSize of
% them. So there is always a gap between captures; it is shown in the plot
% (the lines are broken) instead of being hidden. For gapless recording see
% the libusb path (libusb/tetbench_libusb.m).
clear all
close all

bufferSize = 1e5;               % samples per capture, 1..1047552 (limit inside HTMarch.dll)
DEVICE_INDEX = uint16(0);       % First connected device
DEVICE_TYPE = uint16(1);        % argument of dsoChooseDevice (HTMarch only checks 0 / non-0)
SAMPLE_RATE = int16(0);         % nTimeDIV index of HTMarch.dll, see SamplingLUT below:
                                % 0-10 = 48 MSa/s, 11 = 16 MSa/s, 12 = 8 MSa/s, 13 = 4 MSa/s,
                                % 14-24 = 1 MSa/s, 25 = 500 kSa/s, 26 = 200 kSa/s, 27-38 = 100 kSa/s
VOLT_DIV = [5 5];               % nVoltDIV for CH1, CH2: 0 = 20 mV, 1 = 50 mV, 2 = 100 mV, 3 = 200 mV,
                                % 4 = 500 mV, 5 = 1 V, 6 = 2 V, 7 = 5 V per div. Only the hardware
                                % gain matters for the data: 10,10,10,5,2,1,1,1 -> the input range is
                                % about +-5.12 V / gain (e.g. 5 -> +-5.12 V, 4 -> +-2.56 V, 2 -> +-0.51 V)
BUFFER_SIZE = uint32(bufferSize);     % Number of samples to capture
NUM_CAPTURES = 2;              % Number of captures (each one is a separate acquisition)
SAVE_CALIBRATION = true;       % save the scope's factory zero levels to hantek_cal.mat
                               % (the libusb path uses them: its firmware cannot read the EEPROM)
SamplingRate = SamplingLUT(SAMPLE_RATE);

MAX_READ_LEN = 1047552;         % HTMarch captures 1048576 samples and drops the first 1024
SKIPPED_SAMPLES = 1024;
if bufferSize < 1 || bufferSize > MAX_READ_LEN || bufferSize ~= round(bufferSize)
    error('bufferSize must be an integer 1..%d', MAX_READ_LEN);
end
if numel(VOLT_DIV) ~= 2 || any(VOLT_DIV < 0 | VOLT_DIV > 7 | VOLT_DIV ~= round(VOLT_DIV))
    error('VOLT_DIV must be two integers 0..7');
end
fprintf('Sample rate %g Sa/s: every capture takes at least %.3f s (1048576 samples are recorded)\n', ...
    SamplingRate, 1048576 / SamplingRate);

try
    %% Initialize & Configure Device
    cal = InitHantek6022(DEVICE_INDEX, DEVICE_TYPE, SAMPLE_RATE, VOLT_DIV);
    if SAVE_CALIBRATION && ~isempty(cal)
        calInfo = ['Factory zero levels of the Hantek 6022 (dsoGetCalLevel, raw ADC counts). ' ...
                   'Index (1-based): 16*hs + 2*nVoltDIV + ch + 1, hs = 1 at 48 MSa/s, ch = 0 CH1 / 1 CH2.'];
        save('hantek_cal.mat', 'cal', 'calInfo');
        fprintf('Calibration saved to %s\n', fullfile(pwd, 'hantek_cal.mat'));
    end
    % Zero level (raw counts) of each channel for the chosen volt/div and rate
    zero = ZeroLevels(cal, VOLT_DIV, SAMPLE_RATE <= 10);
    fprintf('Zero levels: CH1 %g, CH2 %g counts\n', zero(1), zero(2));

    % Preallocate storage for multiple captures
    data = struct(...
        'time', zeros(BUFFER_SIZE, NUM_CAPTURES), ...
        'ch1',  zeros(BUFFER_SIZE, NUM_CAPTURES), ...
        'ch2',  zeros(BUFFER_SIZE, NUM_CAPTURES), ...
        'clipped', zeros(1, 2));

    % Capture loop
    hWait = waitbar(0, 'Capturing data...');
    t0 = tic;
    for i = 1:NUM_CAPTURES
        % Create MATLAB-compatible buffers
        ch1 = libpointer('int16Ptr', zeros(BUFFER_SIZE, 1, 'int16'));
        ch2 = libpointer('int16Ptr', zeros(BUFFER_SIZE, 1, 'int16'));

        % Capture data. The last argument is ignored by HantekProxy.exe
        % (see the comment there); the sample rate comes from dsoSetTimeDIV.
        tCall = toc(t0);
        ret = calllib('HantekWrapper', 'dsoReadHardData_LA', ...
            DEVICE_INDEX, ch1, ch2, BUFFER_SIZE, SAMPLE_RATE);
        if ret ~= 1
            error('Capture %d failed: dsoReadHardData_LA returned %d (-1 = scope/pipe error, -2 = rejected request; see HantekProxy.log)', i, ret);
        end

        % Time axis in ms from the host clock: each capture starts when its
        % call reached the scope (+ the 1024 samples HTMarch skips). The
        % start of a capture is only known to a few ms; within a capture the
        % spacing is exact.
        data.time(:,i) = 1e3 * (tCall + (SKIPPED_SAMPLES + (0:double(BUFFER_SIZE)-1)') / SamplingRate);
        raw1 = double(ch1.Value);
        raw2 = double(ch2.Value);
        data.clipped = data.clipped + [sum(raw1 <= 0 | raw1 >= 255), sum(raw2 <= 0 | raw2 >= 255)];
        data.ch1(:,i) = Raw2Volts(raw1, zero(1), VOLT_DIV(1));
        data.ch2(:,i) = Raw2Volts(raw2, zero(2), VOLT_DIV(2));

        waitbar(i/NUM_CAPTURES, hWait, sprintf('Capture %d/%d', i, NUM_CAPTURES));
    end
    close(hWait);
    AnalyzeData(data);

catch ME    % Comprehensive error reporting
    if exist('hWait', 'var') && isvalid(hWait)
        delete(hWait);
    end
    fprintf('\nERROR: %s\n', ME.message);
    for st = ME.stack'
        fprintf('  In %s (line %d)\n', st.name, st.line);
    end
end

%% Clean up
stopHantekProxy

function AnalyzeData(data)
    figure('Name', 'Hantek 6022BL Data', 'NumberTitle', 'off');

    % One line segment per capture: a NaN row between captures breaks the
    % line, so the gaps between acquisitions stay visible.
    n = size(data.time, 2);
    t = [data.time; nan(1, n)];
    y1 = [data.ch1; nan(1, n)];
    y2 = [data.ch2; nan(1, n)];

    subplot(2,1,1);
    plot(t(:), y1(:));
    title('Channel 1');
    xlabel('Time (ms)');
    ylabel('Voltage (V)');
    grid on;

    subplot(2,1,2);
    plot(t(:), y2(:));
    title('Channel 2');
    xlabel('Time (ms)');
    ylabel('Voltage (V)');
    grid on;

    % Statistics
    fprintf('\nCapture Statistics:\n');
    fprintf('  Total captures: %d\n', size(data.ch1, 2));
    fprintf('  Samples per capture: %d\n', size(data.ch1, 1));
    if n > 1
        gaps = data.time(1, 2:end) - data.time(end, 1:end-1);
        fprintf('  Gap between captures: %.1f .. %.1f ms\n', min(gaps), max(gaps));
    end
    fprintf('  CH1 Range: [%.3f, %.3f] V\n', min(data.ch1(:)), max(data.ch1(:)));
    fprintf('  CH2 Range: [%.3f, %.3f] V\n', min(data.ch2(:)), max(data.ch2(:)));
    if any(data.clipped)
        warning(['Clipping: %d (CH1) / %d (CH2) samples at the ADC limits 0 or 255. ' ...
                 'Use a larger VOLT_DIV (lower gain) for that channel.'], ...
            data.clipped(1), data.clipped(2));
    end
end

%% startHantekProxy
% Starts the HantekProxy.exe application and loads the HantekWrapper.dll library
function startHantekProxy()
    exePath = fullfile(pwd, 'HantekProxy.exe');
    if ~isfile(exePath)
        error('HantekProxy.exe not found in the Current Folder (%s)', pwd);
    end
    [status, cmdout] = system('tasklist /FO CSV /NH /FI "IMAGENAME eq HantekProxy.exe"');
    if ~status && contains(cmdout, 'HantekProxy.exe')
        fprintf("HantekProxy.exe is already running\n");
    else
        % Started through .NET: returns at once and opens no window
        % (system('start /B ...') can keep MATLAB waiting on the proxy's output).
        % Working folder = Current Folder, so HantekProxy.log goes there.
        % connectToProxy() then waits up to ~5 s for the proxy to come up.
        psi = System.Diagnostics.ProcessStartInfo(exePath);
        psi.WorkingDirectory = pwd;
        psi.UseShellExecute = false;
        psi.CreateNoWindow = true;
        System.Diagnostics.Process.Start(psi);
        fprintf('HantekProxy.exe started\n');
    end
    if libisloaded('HantekWrapper')
        fprintf("Library already loaded\n");
    else
        [notfound, warnings] = loadlibrary('HantekWrapper.dll', 'HantekWrapper.h');
        if ~isempty(notfound)
            error('Missing critical functions: %s', strjoin(notfound, ', '));
        end
        if ~isempty(warnings)
            warning('Library warnings:\n%s', strjoin(warnings, '\n'));
        end
        fprintf('Library loaded successfully. Available functions:\n');
        libfunctions('HantekWrapper', '-full');
    end
end

%% stopHantekProxy
% Terminates the connection, the HantekProxy.exe application and unloads the library
function stopHantekProxy()
    if libisloaded('HantekWrapper')
        calllib('HantekWrapper', 'disconnectFromProxy');
        unloadlibrary('HantekWrapper');
        if ~libisloaded('HantekWrapper')
            fprintf('HantekWrapper.dll unloaded\n');
        else
            fprintf("Could not unload the HantekWrapper Library\n")
        end
    else
        fprintf('the library wasnt open or found\n')
    end

    [status, cmdout] = system('tasklist /FO CSV /NH /FI "IMAGENAME eq HantekProxy.exe"');
    if ~status && contains(cmdout, 'HantekProxy.exe')
        system('taskkill /IM HantekProxy.exe /F');
        [status, cmdout] = system('tasklist /FO CSV /NH /FI "IMAGENAME eq HantekProxy.exe"');
        if ~status && ~contains(cmdout, 'HantekProxy.exe')
            fprintf("HantekProxy Terminated successfully\n");
        else
            fprintf("Could not terminate the HantekProxy.exe\n")
        end
    else
        fprintf('The HantekProxy wasnt open or found.\n')
    end
end

%% InitHantek6022
% Connects to HantekProxy.exe, opens the instrument, selects the mode, sets the
% sampling rate and volt/div and reads the factory calibration (empty if it
% could not be read). Throws an error if any setting fails.
function cal = InitHantek6022(DEVICE_INDEX, DEVICE_TYPE, SAMPLE_RATE, VOLT_DIV)
    startHantekProxy();
    fprintf('\nInitializing device...\n');
    if calllib('HantekWrapper', 'connectToProxy') ~= 1
        error('Could not connect to HantekProxy.exe (is it running? see HantekProxy.log)');
    end
    ret = calllib('HantekWrapper', 'dsoOpenDevice', DEVICE_INDEX);
    if ret ~= 1
        error('dsoOpenDevice returned %d: is the 6022BL connected and the Hantek driver installed?', ret);
    end
    ret = calllib('HantekWrapper', 'dsoChooseDevice', DEVICE_INDEX, DEVICE_TYPE);
    if ret ~= 1
        error('dsoChooseDevice returned %d', ret);
    end
    pause(0.8);
    ret = calllib('HantekWrapper', 'dsoSetTimeDIV', DEVICE_INDEX, SAMPLE_RATE);
    if ret ~= 1
        error('dsoSetTimeDIV(%d) returned %d', SAMPLE_RATE, ret);
    end
    for ch = 1:2
        ret = calllib('HantekWrapper', 'dsoSetVoltDIV', DEVICE_INDEX, ch - 1, VOLT_DIV(ch));
        if ret ~= 1
            error('dsoSetVoltDIV(CH%d, %d) returned %d', ch, VOLT_DIV(ch), ret);
        end
    end
    [ret, cal] = calllib('HantekWrapper', 'dsoGetCalLevel', DEVICE_INDEX, zeros(32, 1, 'int16'), int16(32));
    if ret == 1
        cal = double(cal);
    else
        warning('dsoGetCalLevel returned %d: using the nominal zero level 128', ret);
        cal = [];
    end
    fprintf('Device is open. Ready for use\n');
end

function zero = ZeroLevels(cal, VOLT_DIV, highSpeed)
    % Zero level of CH1 and CH2 in raw counts. The factory levels are stored
    % like HTMarch.dll's dsoReadHardData uses them: cal(16*hs + 2*nVoltDIV + ch + 1).
    zero = [128 128];
    if isempty(cal)
        return
    end
    for ch = 1:2
        z = cal(16 * highSpeed + 2 * VOLT_DIV(ch) + ch);
        if z >= 96 && z <= 160
            zero(ch) = z;
        else
            warning('Calibration level %g for CH%d looks wrong (EEPROM empty?): using 128', z, ch);
        end
    end
end

function volts = Raw2Volts(raw, zero, voltDiv)
    % Raw ADC counts -> volts. HTMarch.dll's own scale factors correspond to
    % 25 counts per volt at gain 1 (+-5.12 V full scale), 25*gain in general.
    % This is the nominal value; check it once against a known voltage.
    GAIN = [10 10 10 5 2 1 1 1];           % hardware gain per nVoltDIV (from HTMarch.dll)
    COUNTS_PER_VOLT = 25;
    volts = (double(raw) - zero) / (COUNTS_PER_VOLT * GAIN(voltDiv + 1));
end

function samplingRate = SamplingLUT(timeDiv)
    % nTimeDIV -> sample rate, exactly as implemented inside HTMarch.dll:
    % dsoSetTimeDIV looks the index up in a 39-entry table and sends the
    % resulting rate code (48, 16, 8, 4, 1, 50, 20 or 10) to the scope.
    % Indices >= 39 are rejected by the DLL.
    rates = [repmat(48e6, 1, 11), 16e6, 8e6, 4e6, repmat(1e6, 1, 11), ...
             500e3, 200e3, repmat(100e3, 1, 12)];
    timeDiv = double(timeDiv);
    if timeDiv < 0 || timeDiv > numel(rates) - 1 || timeDiv ~= round(timeDiv)
        error('nTimeDIV must be an integer 0..%d, got %g', numel(rates) - 1, timeDiv);
    end
    samplingRate = rates(timeDiv + 1);
end
