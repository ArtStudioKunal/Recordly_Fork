#include "wgc_session.h"
#include "mf_encoder.h"
#include "monitor_utils.h"
#include "wasapi_loopback.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>

#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <vector>
#include <algorithm>

static std::atomic<bool> g_stopRequested{false};
static std::atomic<bool> g_pauseRequested{false};
static std::atomic<int64_t> g_lastFrameTimestampHns{0};
static std::atomic<int64_t> g_pauseStartTimestampHns{0};
static std::atomic<int64_t> g_accumulatedPausedHns{0};
static std::mutex g_stopMutex;
static std::condition_variable g_stopCv;

static void reportMicrophoneCaptureUnavailable() {
    std::cerr << "WARNING: Failed to initialize WASAPI mic capture" << std::endl;
    // This stable stdout marker is ordered before "Recording started", allowing
    // Electron to select its per-recording browser fallback without a pipe race.
    std::cout << "MICROPHONE_CAPTURE_UNAVAILABLE" << std::endl;
    std::cout.flush();
}

struct CaptureConfig {
    int64_t displayId = 0;
    int64_t windowHandle = 0;
    std::string outputPath;
    std::string audioOutputPath;
    std::string micOutputPath;
    std::string micDeviceId;
    std::string micDeviceName;
    int fps = 60;
    int width = 0;
    int height = 0;
    int displayX = 0;
    int displayY = 0;
    int displayW = 0;
    int displayH = 0;
    bool hasDisplayBounds = false;
    bool captureSystemAudio = false;
    bool captureMic = false;
    bool captureAllMonitors = false;
};

static bool parseSimpleJson(const std::string& json, CaptureConfig& config) {
    auto findInt = [&](const std::string& key) -> int {
        auto pos = json.find("\"" + key + "\"");
        if (pos == std::string::npos) return -1;
        pos = json.find(':', pos);
        if (pos == std::string::npos) return -1;
        pos++;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
        try {
            return std::stoi(json.substr(pos));
        } catch (...) {
            return -1;
        }
    };

    auto findInt64 = [&](const std::string& key) -> int64_t {
        auto pos = json.find("\"" + key + "\"");
        if (pos == std::string::npos) return -1;
        pos = json.find(':', pos);
        if (pos == std::string::npos) return -1;
        pos++;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
        try {
            return std::stoll(json.substr(pos));
        } catch (...) {
            return -1;
        }
    };

    auto findString = [&](const std::string& key) -> std::string {
        auto pos = json.find("\"" + key + "\"");
        if (pos == std::string::npos) return "";
        pos = json.find(':', pos);
        if (pos == std::string::npos) return "";
        pos++;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
        if (pos >= json.size() || json[pos] != '"') return "";
        pos++;
        std::string result;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) {
                pos++;
                if (json[pos] == 'n') result += '\n';
                else if (json[pos] == 't') result += '\t';
                else if (json[pos] == '\\') result += '\\';
                else if (json[pos] == '"') result += '"';
                else if (json[pos] == '/') result += '/';
                else result += json[pos];
            } else {
                result += json[pos];
            }
            pos++;
        }
        return result;
    };

    config.outputPath = findString("outputPath");
    if (config.outputPath.empty()) return false;

    int64_t displayId = findInt64("displayId");
    if (displayId >= 0) config.displayId = displayId;

    int64_t windowHandle = findInt64("windowHandle");
    if (windowHandle > 0) config.windowHandle = windowHandle;

    int fps = findInt("fps");
    if (fps > 0) config.fps = fps;

    int width = findInt("width");
    if (width > 0) config.width = width;

    int height = findInt("height");
    if (height > 0) config.height = height;

    config.audioOutputPath = findString("audioOutputPath");
    config.micOutputPath = findString("micOutputPath");
    config.micDeviceId = findString("micDeviceId");
    config.micDeviceName = findString("micDeviceName");

    auto findBool = [&](const std::string& key) -> bool {
        auto pos = json.find("\"" + key + "\"");
        if (pos == std::string::npos) return false;
        auto colonPos = json.find(':', pos);
        if (colonPos == std::string::npos) return false;
        auto valStart = json.find_first_not_of(" \t", colonPos + 1);
        return valStart != std::string::npos && json.substr(valStart, 4) == "true";
    };

    config.captureSystemAudio = findBool("captureSystemAudio");
    config.captureMic = findBool("captureMic");
    config.captureAllMonitors = findBool("captureAllMonitors");

    int dx = findInt("displayX");
    int dy = findInt("displayY");
    int dw = findInt("displayW");
    int dh = findInt("displayH");
    if (dw > 0 && dh > 0) {
        config.displayX = dx;
        config.displayY = dy;
        config.displayW = dw;
        config.displayH = dh;
        config.hasDisplayBounds = true;
    }

    return true;
}

static std::wstring utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    std::wstring wstr(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), &wstr[0], len);
    return wstr;
}

static int64_t queryPerformanceCounterHns() {
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
        return g_lastFrameTimestampHns.load();
    }

    return static_cast<int64_t>(
        (static_cast<long double>(counter.QuadPart) * 10000000.0L) /
        static_cast<long double>(frequency.QuadPart));
}

static int64_t adjustedVideoTimestampHns(int64_t timestampHns) {
    int64_t accumulatedPausedHns = g_accumulatedPausedHns.load();
    if (g_pauseRequested.load()) {
        const int64_t pauseStart = g_pauseStartTimestampHns.load();
        if (pauseStart > 0 && timestampHns > pauseStart) {
            accumulatedPausedHns += (timestampHns - pauseStart);
        }
    }

    int64_t adjustedTimestampHns = timestampHns - accumulatedPausedHns;
    if (adjustedTimestampHns < 0) {
        adjustedTimestampHns = 0;
    }
    return adjustedTimestampHns;
}

static void openActivePauseAt(int64_t timestampHns) {
    g_pauseStartTimestampHns.store(timestampHns);
    g_pauseRequested.store(true);
}

static void closeActivePauseAt(int64_t timestampHns) {
    const int64_t pauseStart = g_pauseStartTimestampHns.exchange(0);
    if (pauseStart > 0 && timestampHns > pauseStart) {
        g_accumulatedPausedHns.fetch_add(timestampHns - pauseStart);
    }
    g_pauseRequested.store(false);
}

static void writeCompanionAudioTimingMetadata(
    const std::string& audioPath,
    int64_t firstVideoTimestampHns,
    const WasapiCapture& capture
) {
    if (audioPath.empty() || firstVideoTimestampHns < 0) {
        return;
    }

    const int64_t firstPacketQpcHns = capture.firstPacketQpcHns();
    if (firstPacketQpcHns < 0) {
        return;
    }

    int64_t startDelayMs = (firstPacketQpcHns - firstVideoTimestampHns + 5000) / 10000;
    if (startDelayMs < 0) {
        startDelayMs = 0;
    }

    std::ofstream metadataFile(audioPath + ".json", std::ios::trunc);
    if (!metadataFile) {
        std::cerr << "WARNING: Failed to write audio timing metadata for " << audioPath << std::endl;
        return;
    }

    metadataFile << "{\"startDelayMs\":" << startDelayMs;
    metadataFile << ",\"capturedDurationMs\":" << capture.capturedDurationMs();
    metadataFile << ",\"dataBytes\":" << capture.totalDataBytes();
    metadataFile << ",\"sampleRate\":" << capture.sampleRate();
    metadataFile << ",\"channels\":" << capture.channelCount();
    metadataFile << ",\"gapFillCount\":" << capture.gapFillCount();
    metadataFile << ",\"insertedSilenceFrames\":" << capture.insertedSilenceFrames();
    metadataFile << ",\"compactedDiscontinuityCount\":" << capture.compactedDiscontinuityCount();
    metadataFile << ",\"compactedDiscontinuityFrames\":" << capture.compactedDiscontinuityFrames();
    metadataFile << ",\"compactedSilentDiscontinuityCount\":" << capture.compactedSilentDiscontinuityCount();
    metadataFile << ",\"compactedSilentDiscontinuityFrames\":" << capture.compactedSilentDiscontinuityFrames();
    const uint32_t discontinuityCount = capture.dataDiscontinuityCount();
    if (discontinuityCount > 0) {
        metadataFile << ",\"dataDiscontinuityCount\":" << discontinuityCount;
    }
    const uint32_t timestampErrorCount = capture.timestampErrorCount();
    if (timestampErrorCount > 0) {
        metadataFile << ",\"timestampErrorCount\":" << timestampErrorCount;
    }
    metadataFile << "}";
}

static void stdinListenerThread() {
    std::string line;
    while (std::getline(std::cin, line)) {
        // Trim whitespace
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
            line.pop_back();
        }

        if (line == "pause") {
            openActivePauseAt(queryPerformanceCounterHns());
            continue;
        }

        if (line == "resume") {
            closeActivePauseAt(queryPerformanceCounterHns());
            continue;
        }

        if (line == "stop") {
            g_stopRequested = true;
            g_stopCv.notify_all();
            return;
        }
    }

    // stdin closed (parent process died)
    g_stopRequested = true;
    g_stopCv.notify_all();
}


/**
 * Windows virtual-desktop capture path. BitBlt copies the composed desktop
 * surface (including monitors with negative coordinates) into one BGRA frame.
 * The existing Media Foundation encoder and WASAPI sidecars are reused.
 * This backend is deliberately separate from WGC window capture.
 */
static int recordVirtualDesktop(const CaptureConfig& config) {
    const int originX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int originY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int physicalWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int physicalHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (physicalWidth < 2 || physicalHeight < 2) {
        std::cerr << "ERROR: Invalid virtual desktop dimensions" << std::endl;
        return 1;
    }
    const int width = physicalWidth & ~1;
    const int height = physicalHeight & ~1;
    if (width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
        std::cerr << "ERROR: Combined monitor dimensions exceed D3D11 limit" << std::endl;
        return 1;
    }
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &device, &level, &context);
    if (FAILED(hr)) {
        std::cerr << "ERROR: Cannot initialize virtual desktop D3D device" << std::endl;
        return 1;
    }
    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&textureDesc, nullptr, &texture))) return 1;

    HDC desktopDc = GetDC(nullptr);
    if (!desktopDc) return 1;
    HDC memoryDc = CreateCompatibleDC(desktopDc);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(desktopDc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!memoryDc || !bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        if (memoryDc) DeleteDC(memoryDc);
        ReleaseDC(nullptr, desktopDc);
        return 1;
    }
    HGDIOBJ previous = SelectObject(memoryDc, bitmap);
    MFEncoder encoder;
    const std::wstring output = utf8ToWide(config.outputPath);
    if (!encoder.initialize(output, width, height, config.fps, device.Get(), context.Get())) {
        SelectObject(memoryDc, previous);
        DeleteObject(bitmap);
        DeleteDC(memoryDc);
        ReleaseDC(nullptr, desktopDc);
        return 1;
    }

    WasapiCapture loopback;
    WasapiCapture mic;
    bool audioActive = false;
    bool micActive = false;
    const bool audioReady = config.captureSystemAudio &&
        !config.audioOutputPath.empty() && loopback.initializeLoopback(config.audioOutputPath);
    const bool micReady = config.captureMic && !config.micOutputPath.empty() &&
        mic.initializeMic(config.micOutputPath, config.micDeviceId, config.micDeviceName);
    if (config.captureMic && !micReady) reportMicrophoneCaptureUnavailable();

    std::thread(stdinListenerThread).detach();
    if (audioReady) audioActive = loopback.start();
    if (micReady) {
        micActive = mic.start();
        if (!micActive) reportMicrophoneCaptureUnavailable();
    }
    const auto framePeriod = std::chrono::nanoseconds(1000000000LL / (std::max)(1, config.fps));
    auto nextFrame = std::chrono::steady_clock::now();
    int64_t firstTimestamp = -1;
    int64_t capturedFrames = 0;
    bool failed = false;
    while (!g_stopRequested) {
        if (g_pauseRequested) {
            if (audioActive) loopback.pause();
            if (micActive) mic.pause();
        } else {
            if (audioActive) loopback.resume();
            if (micActive) mic.resume();
            if (!BitBlt(memoryDc, 0, 0, width, height, desktopDc, originX, originY,
                        SRCCOPY | CAPTUREBLT)) {
                failed = true;
                break;
            }
            context->UpdateSubresource(texture.Get(), 0, nullptr, pixels, width * 4, 0);
            const int64_t timestamp = queryPerformanceCounterHns();
            if (firstTimestamp < 0) firstTimestamp = timestamp;
            if (!encoder.writeFrame(texture.Get(), adjustedVideoTimestampHns(timestamp))) {
                failed = true;
                break;
            }
            ++capturedFrames;
            if (capturedFrames == 1) {
                std::cout << "Recording started" << std::endl;
                std::cout.flush();
            }
        }
        nextFrame += framePeriod;
        const auto now = std::chrono::steady_clock::now();
        if (nextFrame < now) nextFrame = now;
        std::this_thread::sleep_until(nextFrame);
    }
    const int64_t stopTimestamp = adjustedVideoTimestampHns(queryPerformanceCounterHns());
    if (audioActive) loopback.stop();
    if (micActive) mic.stop();
    if (audioActive && firstTimestamp >= 0)
        writeCompanionAudioTimingMetadata(config.audioOutputPath, firstTimestamp, loopback);
    if (micActive && firstTimestamp >= 0)
        writeCompanionAudioTimingMetadata(config.micOutputPath, firstTimestamp, mic);
    if (capturedFrames > 0) encoder.extendLastFrameTo(stopTimestamp);
    const bool finalized = encoder.finalize();
    SelectObject(memoryDc, previous);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, desktopDc);
    if (failed || !finalized || capturedFrames == 0) {
        DeleteFileW(output.c_str());
        std::cerr << "ERROR: Virtual desktop capture failed" << std::endl;
        return 1;
    }
    std::cout << "Recording stopped. Output path: " << config.outputPath << std::endl;
    std::cout.flush();
    Sleep(100);
    ExitProcess(0);
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "ERROR: Missing JSON config argument" << std::endl;
        return 1;
    }

    // Keep Win32 monitor/window rectangles in physical pixels. Without
    // per-monitor awareness, mixed-DPI desktops can report virtualized bounds
    // that do not line up with the WGC monitor texture.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    CaptureConfig config;
    if (!parseSimpleJson(argv[1], config)) {
        std::cerr << "ERROR: Failed to parse config JSON" << std::endl;
        return 1;
    }

    if (config.captureAllMonitors) return recordVirtualDesktop(config);

    WgcSession session;

    if (config.windowHandle > 0) {
        HWND hwnd = reinterpret_cast<HWND>(static_cast<intptr_t>(config.windowHandle));
        if (!IsWindow(hwnd)) {
            std::cerr << "ERROR: Invalid window handle " << config.windowHandle << std::endl;
            return 1;
        }
        if (!session.initialize(hwnd, config.fps)) {
            std::cerr << "ERROR: Failed to initialize WGC window capture session" << std::endl;
            return 1;
        }
    } else {
        HMONITOR monitor = findMonitorByDisplayId(config.displayId);
        if (!monitor && config.hasDisplayBounds) {
            std::cerr << "Monitor ID match failed, attempting coordinate-based match: "
                      << config.displayX << "," << config.displayY << " " << config.displayW << "x" << config.displayH << std::endl;
            monitor = findMonitorByBounds(config.displayX, config.displayY, config.displayW, config.displayH);
        }

        if (!monitor) {
            std::cerr << "ERROR: Could not find monitor for displayId " << config.displayId << std::endl;
            return 1;
        }
        if (!session.initialize(monitor, config.fps)) {
            std::cerr << "ERROR: Failed to initialize WGC capture session" << std::endl;
            return 1;
        }
    }

    int captureWidth = config.width > 0 ? config.width : session.captureWidth();
    int captureHeight = config.height > 0 ? config.height : session.captureHeight();

    // Ensure even dimensions for H.264
    captureWidth = (captureWidth / 2) * 2;
    captureHeight = (captureHeight / 2) * 2;

    // Initialize encoder
    MFEncoder encoder;
    std::wstring outputPathW = utf8ToWide(config.outputPath);
    if (!encoder.initialize(outputPathW, captureWidth, captureHeight, config.fps,
                           session.device(), session.context())) {
        std::cerr << "ERROR: Failed to initialize Media Foundation encoder" << std::endl;
        return 1;
    }

    // Set up frame callback
    std::atomic<int64_t> frameCount{0};
    std::atomic<int64_t> firstVideoTimestampHns{-1};
    std::atomic<bool> recordingStartedAnnounced{false};
    std::atomic<bool> captureSetupComplete{false};
    session.setFrameCallback([&](ID3D11Texture2D* texture, int64_t timestampHns) {
        g_lastFrameTimestampHns = timestampHns;
        int64_t expectedFirstVideoTimestampHns = -1;
        firstVideoTimestampHns.compare_exchange_strong(expectedFirstVideoTimestampHns, timestampHns);
        if (g_stopRequested) return;

        if (g_pauseRequested) return;

        const int64_t adjustedTimestampHns = adjustedVideoTimestampHns(timestampHns);

        if (encoder.writeFrame(texture, adjustedTimestampHns)) {
            const int64_t writtenFrames = frameCount.fetch_add(1) + 1;
            if (captureSetupComplete && writtenFrames >= 1 && !recordingStartedAnnounced.exchange(true)) {
                std::cout << "Recording started" << std::endl;
                std::cout.flush();
            }
        }
    });

    // Start stdin listener
    std::thread stdinThread(stdinListenerThread);
    stdinThread.detach();

    // Initialize WASAPI captures (but don't start yet)
    WasapiCapture loopback;
    WasapiCapture micCapture;
    bool audioActive = false;
    bool audioInitialized = false;
    bool micActive = false;
    bool micInitialized = false;

    if (config.captureSystemAudio && !config.audioOutputPath.empty()) {
        audioInitialized = loopback.initializeLoopback(config.audioOutputPath);
        if (!audioInitialized) {
            std::cerr << "WARNING: Failed to initialize WASAPI loopback" << std::endl;
        }
    }

    if (config.captureMic && !config.micOutputPath.empty()) {
        micInitialized = micCapture.initializeMic(
            config.micOutputPath,
            config.micDeviceId,
            config.micDeviceName);
        if (!micInitialized) {
            reportMicrophoneCaptureUnavailable();
        }
    }

    // Start video capture, then audio immediately after for sync
    if (!session.startCapture()) {
        std::cerr << "ERROR: Failed to start WGC capture" << std::endl;
        return 1;
    }

    if (audioInitialized) {
        audioActive = loopback.start();
    }
    if (micInitialized) {
        micActive = micCapture.start();
        if (!micActive) {
            reportMicrophoneCaptureUnavailable();
        }
    }
    captureSetupComplete = true;

    // Wait for stop signal while pausing/resuming audio tracks in lockstep.
    while (!g_stopRequested && !session.hasFatalError()) {
        if (g_pauseRequested) {
            if (audioActive) loopback.pause();
            if (micActive) micCapture.pause();
        } else {
            if (audioActive) loopback.resume();
            if (micActive) micCapture.resume();
        }

        std::unique_lock<std::mutex> lock(g_stopMutex);
        g_stopCv.wait_for(lock, std::chrono::milliseconds(20), [] { return g_stopRequested.load(); });
    }

    // Stop capture and finalize
    const int64_t adjustedStopTimestampHns = adjustedVideoTimestampHns(queryPerformanceCounterHns());
    session.stopCapture();
    if (audioActive) loopback.stop();
    if (micActive) micCapture.stop();

    if (session.hasFatalError()) {
        std::cerr << "ERROR: WGC capture session failed during recording" << std::endl;
        encoder.finalize();
        DeleteFileW(outputPathW.c_str());
        if (!config.audioOutputPath.empty()) {
            const std::wstring audioPathW = utf8ToWide(config.audioOutputPath);
            const std::wstring audioMetadataPathW = utf8ToWide(config.audioOutputPath + ".json");
            DeleteFileW(audioPathW.c_str());
            DeleteFileW(audioMetadataPathW.c_str());
        }
        if (!config.micOutputPath.empty()) {
            const std::wstring micPathW = utf8ToWide(config.micOutputPath);
            const std::wstring micMetadataPathW = utf8ToWide(config.micOutputPath + ".json");
            DeleteFileW(micPathW.c_str());
            DeleteFileW(micMetadataPathW.c_str());
        }
        return 1;
    }

    if (audioActive) {
        writeCompanionAudioTimingMetadata(
            config.audioOutputPath,
            firstVideoTimestampHns.load(),
            loopback);
    }
    if (micActive) {
        writeCompanionAudioTimingMetadata(
            config.micOutputPath,
            firstVideoTimestampHns.load(),
            micCapture);
    }

    if (frameCount.load() <= 0) {
        std::cerr << "ERROR: No video frames were captured before stop" << std::endl;
        DeleteFileW(outputPathW.c_str());
        return 1;
    }

    if (!encoder.extendLastFrameTo(adjustedStopTimestampHns)) {
        std::cerr << "WARNING: Failed to extend the last video frame to the stop timestamp" << std::endl;
    }

    if (!encoder.finalize()) {
        std::cerr << "ERROR: Failed to finalize Media Foundation encoder" << std::endl;
        return 1;
    }

    std::cout << "Recording stopped. Output path: " << config.outputPath << std::endl;
    if (audioActive) {
        if (
            loopback.dataDiscontinuityCount() > 0 ||
            loopback.timestampErrorCount() > 0 ||
            loopback.gapFillCount() > 0 ||
            loopback.compactedDiscontinuityCount() > 0 ||
            loopback.compactedSilentDiscontinuityCount() > 0
        ) {
            std::cerr << "WARNING: System audio timing metadata includes discontinuities="
                      << loopback.dataDiscontinuityCount()
                      << " timestampErrors=" << loopback.timestampErrorCount()
                      << " gapFills=" << loopback.gapFillCount()
                      << " insertedSilenceFrames=" << loopback.insertedSilenceFrames()
                      << " compactedDiscontinuities=" << loopback.compactedDiscontinuityCount()
                      << " compactedDiscontinuityFrames=" << loopback.compactedDiscontinuityFrames()
                      << " compactedSilentDiscontinuities=" << loopback.compactedSilentDiscontinuityCount()
                      << " compactedSilentDiscontinuityFrames=" << loopback.compactedSilentDiscontinuityFrames()
                      << std::endl;
        }
        std::cout << "Audio path: " << config.audioOutputPath << std::endl;
    }
    if (micActive) {
        if (
            micCapture.dataDiscontinuityCount() > 0 ||
            micCapture.timestampErrorCount() > 0 ||
            micCapture.gapFillCount() > 0 ||
            micCapture.compactedDiscontinuityCount() > 0 ||
            micCapture.compactedSilentDiscontinuityCount() > 0
        ) {
            std::cerr << "WARNING: Microphone timing metadata includes discontinuities="
                      << micCapture.dataDiscontinuityCount()
                      << " timestampErrors=" << micCapture.timestampErrorCount()
                      << " gapFills=" << micCapture.gapFillCount()
                      << " insertedSilenceFrames=" << micCapture.insertedSilenceFrames()
                      << " compactedDiscontinuities=" << micCapture.compactedDiscontinuityCount()
                      << " compactedDiscontinuityFrames=" << micCapture.compactedDiscontinuityFrames()
                      << " compactedSilentDiscontinuities=" << micCapture.compactedSilentDiscontinuityCount()
                      << " compactedSilentDiscontinuityFrames=" << micCapture.compactedSilentDiscontinuityFrames()
                      << std::endl;
        }
        std::cout << "Mic path: " << config.micOutputPath << std::endl;
    }
    std::cout.flush();

    // Allow pipe buffers to drain before forceful exit
    Sleep(100);

    // Fast exit to avoid WinRT/COM teardown crashes during apartment cleanup
    ExitProcess(0);
}
