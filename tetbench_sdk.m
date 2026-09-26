% Hantek 6022BL MATLAB Interface - SDK path (HantekWrapper.dll -> HantekProxy.exe -> HTMarch.dll)
%
% Plain-script version of tetbench.mlx (tetbench.mlx itself is left unchanged).
% Right-click -> "Open as Live Script" if you prefer the live editor.
%
% Needs in the Current Folder: HantekWrapper.dll, HantekWrapper.h,
% HantekProxy.exe and HTMarch.dll, the Hantek driver installed and the
% Hantek 6022BL connected.
clear all
close all

bufferSize = 1e5;               % max 1047552 samples per capture (limit inside HTMarch.dll)
DEVICE_INDEX = uint16(0);       % First connected device
DEVICE_TYPE = uint16(1);        % argument of dsoChooseDevice (HTMarch only checks 0 / non-0)
SAMPLE_RATE = int16(0);         % nTimeDIV index of HTMarch.dll, see SamplingLUT below:
                                % 0-10 = 48 MSa/s, 11 = 16 MSa/s, 12 = 8 MSa/s, 13 = 4 MSa/s,
                                % 14-24 = 1 MSa/s, 25 = 500 kSa/s, 26 = 200 kSa/s, 27-38 = 100 kSa/s
BUFFER_SIZE = uint32(bufferSize);     % Number of samples to capture
NUM_CAPTURES = 2;              % Number of captures (each one is a separate acquisition)
SamplingRate = SamplingLUT(SAMPLE_RATE);

%% Initialize & Configure Device
InitHantek6022(DEVICE_INDEX, DEVICE_TYPE, SAMPLE_RATE)

try
    % Preallocate storage for multiple captures
    data = struct(...
        'time', zeros(BUFFER_SIZE, NUM_CAPTURES), ...
        'ch1',  zeros(BUFFER_SIZE, NUM_CAPTURES), ...
        'ch2',  zeros(BUFFER_SIZE, NUM_CAPTURES));

    % Capture loop
    hWait = waitbar(0, 'Capturing data...');
    for i = 1:NUM_CAPTURES
        % Create MATLAB-compatible buffers
        ch1 = libpointer('int16Ptr', zeros(BUFFER_SIZE, 1, 'int16'));
        ch2 = libpointer('int16Ptr', zeros(BUFFER_SIZE, 1, 'int16'));

        % Capture data. The last argument is ignored by HantekProxy.exe
        % (see the comment there); the sample rate comes from dsoSetTimeDIV.
        samples = calllib('HantekWrapper', 'dsoReadHardData_LA', ...
            DEVICE_INDEX, ch1, ch2, BUFFER_SIZE, SAMPLE_RATE);

        if samples == -1
            error('Capture failed on iteration %d', i);
        end

        % Store data with timestamp
        time_offset = 0;
        if i>1
            time_offset = data.time(BUFFER_SIZE,i-1);
        end
        data.time(:,i) = (0:double(BUFFER_SIZE)-1)/(1e-3*SamplingRate) + time_offset; % in ms
        data.ch1(:,i) = (double(ch1.Value) - 128)/2^6;
        data.ch2(:,i) = (double(ch2.Value) - 128)/2^6;

        waitbar(i/NUM_CAPTURES, hWait, sprintf('Capture %d/%d', i, NUM_CAPTURES));
    end
    close(hWait);
    AnalyzeData(data);

catch ME    % Comprehensive error reporting
    fprintf('\nERROR: %s\n', ME.message);
    for st = ME.stack'
        fprintf('  In %s (line %d)\n', st.name, st.line);
    end
end

%% Clean up
stopHantekProxy

function AnalyzeData(data)
    figure('Name', 'Hantek 6022BL Data', 'NumberTitle', 'off');

    % Plot first capture
    subplot(2,1,1);
    disp(length(data.ch1(:,1)))
    plot(reshape(data.time, [],1), reshape(data.ch1, [],1));
    title('Channel 1');
    xlabel('Time (ms)');
    ylabel('Value');
    grid on;

    subplot(2,1,2);
    plot(reshape(data.time, [],1), reshape(data.ch2, [],1));
    title('Channel 2');
    xlabel('Time (ms)');
    ylabel('Value');
    grid on;

    % Statistics
    fprintf('\nCapture Statistics:\n');
    fprintf('  Total captures: %d\n', size(data.ch1, 2));
    fprintf('  Samples per capture: %d\n', size(data.ch1, 1));
    fprintf('  CH1 Range: [%d, %d]\n', min(data.ch1(:)), max(data.ch1(:)));
    fprintf('  CH2 Range: [%d, %d]\n', min(data.ch2(:)), max(data.ch2(:)));
end

%% startHantekProxy
% Starts the HantekProxy.exe application and loads the HantekWrapper.dll library
function startHantekProxy()
    flagEXE = 0;
    flagDLL = 0;
    [status, cmdout] = system('tasklist /FO CSV /FI "IMAGENAME eq HantekProxy.exe"');
    if ~status && contains(cmdout, "PID")
        fprintf("HantekProxy.exe is already running\n");
    else
        system('start /B HantekProxy.exe');
        flagEXE = 1;
    end
    if libisloaded('HantekWrapper')
        fprintf("Library already loaded");
    else
        [notfound, warnings] = loadlibrary('HantekWrapper.dll', 'HantekWrapper.h');
        if ~isempty(notfound)
            error('Missing critical functions: %s', strjoin(notfound, ', '));
        end
        if ~isempty(warnings)
            warning('Library warnings:\n%s', strjoin(warnings, '\n'));
        end
        flagDLL = 1;
    end

    pause(0.8);
    if flagEXE
        [status, cmdout] = system('tasklist /FO CSV /FI "IMAGENAME eq HantekProxy.exe"');
        if ~status && contains(cmdout, "PID") % the .exe is open
            fprintf('Hantek proxy started\n');
        else
            fprintf("Could not start HantekProxy.exe\n");
        end
    end
    if flagDLL
        if libisloaded('HantekWrapper')
            fprintf('Library loaded successfully. Available functions:\n');
            libfunctions('HantekWrapper', '-full');
        else
            fprintf("Could not load the HantekWrapper Library\n")
        end
    end
end

%% stopHantekProxy
% Terminates the connection, the HantekProxy.exe application and unloads the library
function stopHantekProxy() % closes the the .dll and Proxy.exe
    flagEXE = 0;
    flagDLL = 0;
    [status, cmdout] = system('tasklist /FO CSV /FI "IMAGENAME eq HantekProxy.exe"');
    if ~status && contains(cmdout, "PID") % the .exe is open
        calllib('HantekWrapper','disconnectFromProxy');
        system('taskkill /IM HantekProxy.exe /F');
        flagEXE = 1;
    else
        fprintf('The HantekProxy wasnt open or found.\n')
    end

    if libisloaded('HantekWrapper')
        unloadlibrary('HantekWrapper');
        flagDLL = 1;
    else
        fprintf('the library wasnt open or found\n')
    end

    if flagEXE
        [status, cmdout] = system('tasklist /FO CSV /FI "IMAGENAME eq HantekProxy.exe"');
         if ~status && ~contains(cmdout, "PID") % the .exe is open
             fprintf("HantekProxy Teminated successfully\n");
         else
             fprintf("Could not terminate the HantekProxy.exe\n")
         end
    end
    if flagDLL
        if ~libisloaded('HantekWrapper')
            fprintf('HantekWrapper.dll unloaded\n');
        else
            fprintf("Could not unload the HantekWrapper Library\n")
        end
    end
end

%% InitHantek6022
% Connects to HantekProxy.exe, opens the instrument, selects the mode and sets the sampling rate
function InitHantek6022(DEVICE_INDEX, DEVICE_TYPE, SAMPLE_RATE)
    try     % Load library with verbose error reporting
        startHantekProxy();
        fprintf('\nInitializing device...\n');
        calllib('HantekWrapper', 'connectToProxy');
        calllib('HantekWrapper', 'dsoOpenDevice', DEVICE_INDEX);
        calllib('HantekWrapper', 'dsoChooseDevice', DEVICE_INDEX, DEVICE_TYPE);
        pause(0.8);
        calllib('HantekWrapper', 'dsoSetTimeDIV', DEVICE_INDEX, SAMPLE_RATE);
        fprintf('Device is open. Ready for use\n');
    catch ME    % Comprehensive error reporting
        fprintf('\nERROR: %s\n', ME.message);
        for st = ME.stack'
            fprintf('  In %s (line %d)\n', st.name, st.line);
        end
    end
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
