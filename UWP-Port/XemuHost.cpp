#include "pch.h"
#include "XemuHost.h"

#include <robuffer.h>
#include <roapi.h>
#include <windows.storage.h>
#include <windows.ui.xaml.media.dxinterop.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <limits>
#include <sstream>

using namespace UWP_Port;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;
using namespace Windows::Gaming::Input;
using namespace concurrency;

namespace {
struct BrokeredFileStream {
    IRandomAccessStream^ stream;
    std::string name;

    BrokeredFileStream(IRandomAccessStream^ value, const std::string& fileName)
        : stream(value), name(fileName) {}
};
}

XemuHost::XemuHost()
    : m_module(nullptr), m_sdlModule(nullptr), m_openGLModule(nullptr),
      m_mesaModule(nullptr),
      m_running(false), m_stop(false), m_firstFrameLogged(false),
      m_attachMesa(nullptr), m_setMesaSwapChainAttach(nullptr),
      m_updateSDLPanelSize(nullptr), m_attachVirtualJoystick(nullptr),
      m_detachVirtualJoystick(nullptr), m_openJoystick(nullptr),
      m_closeJoystick(nullptr), m_setVirtualAxis(nullptr),
      m_setVirtualButton(nullptr), m_setEmbeddedCursorHidden(nullptr),
      m_setGamepadState(nullptr),
      m_virtualJoystickId(0),
      m_virtualJoystick(nullptr), m_uwpGamepad(nullptr),
      m_gamepadErrorLogged(false), m_lastGamepadTimestamp(0),
      m_gamepadChangeLogs(0), m_renderPanel(nullptr),
      m_getApiVersion(nullptr), m_init(nullptr), m_start(nullptr),
      m_renderFrame(nullptr), m_step(nullptr), m_isHostRunning(nullptr),
      m_requestStop(nullptr), m_pause(nullptr),
      m_resume(nullptr), m_reset(nullptr), m_shutdown(nullptr), m_join(nullptr), m_cleanup(nullptr),
      m_registerLog(nullptr), m_setLogFile(nullptr),
      m_registerBrokeredStorage(nullptr), m_mountFile(nullptr),
      m_mountFolder(nullptr)
{
    m_logPath = (ApplicationData::Current->LocalFolder->Path + L"\\xemu.log")->Data();
    CREATEFILE2_EXTENDED_PARAMETERS params{};
    params.dwSize = sizeof(params);
    params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    HANDLE file = CreateFile2(m_logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              CREATE_ALWAYS, &params);
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
    WriteDiagnostic("[host] New UWP diagnostic session (desktop debug enabled)");
}

XemuHost::~XemuHost()
{
    WriteDiagnostic("[host] Shutting down XemuHost");
    Stop();
    DetachUWPGamepad();
    if (m_module) {
        FreeLibrary(m_module);
    }
    if (m_sdlModule) {
        FreeLibrary(m_sdlModule);
    }
    if (m_openGLModule) {
        FreeLibrary(m_openGLModule);
    }
    if (m_mesaModule) {
        FreeLibrary(m_mesaModule);
    }
}

bool XemuHost::AttachRenderPanel(Windows::UI::Xaml::Controls::SwapChainPanel^ panel)
{
    if (!panel) {
        SetError("xemu SwapChainPanel was not provided");
        return false;
    }

    WriteDiagnostic("[display] Attaching SwapChainPanel to SDL3 and Mesa");
    m_sdlModule = LoadPackagedLibrary(L"SDL3.dll", 0);
    if (!m_sdlModule) {
        WriteDiagnostic("[loader] SDL3.dll failed with Win32 error " +
                        std::to_string(GetLastError()));
    }
    m_mesaModule = LoadPackagedLibrary(L"gallium_wgl.dll", 0);
    if (!m_mesaModule) {
        WriteDiagnostic("[loader] gallium_wgl.dll failed with Win32 error " +
                        std::to_string(GetLastError()));
    }
    m_openGLModule = LoadPackagedLibrary(L"opengl32.dll", 0);
    if (!m_openGLModule) {
        WriteDiagnostic("[loader] opengl32.dll failed with Win32 error " +
                        std::to_string(GetLastError()));
    }
    if (!m_sdlModule || !m_mesaModule || !m_openGLModule) {
        SetError("Failed to load SDL3.dll, opengl32.dll, or gallium_wgl.dll while preparing the renderer");
        return false;
    }

    using AttachSDL = bool (__cdecl *)(void *);
    using SetSDLLog = void (__cdecl *)(
        void (__cdecl *)(void *, int, int, const char *), void *);
    using SetMesaLog = void (__cdecl *)(
        void (__cdecl *)(void *, const char *), void *);
    auto attachSDL = reinterpret_cast<AttachSDL>(
        GetProcAddress(m_sdlModule, "SDL_WinRTAttachXAMLPanel"));
    m_attachMesa = reinterpret_cast<AttachMesa>(
        GetProcAddress(m_mesaModule, "uwp_set_window_reference"));
    m_setMesaSwapChainAttach = reinterpret_cast<SetMesaSwapChainAttach>(
        GetProcAddress(m_mesaModule, "mesa_uwp_set_swapchain_attach_callback"));
    m_updateSDLPanelSize = reinterpret_cast<UpdateSDLPanelSize>(
        GetProcAddress(m_sdlModule, "SDL_WinRTUpdateXAMLPanelSize"));
    auto setSDLLog = reinterpret_cast<SetSDLLog>(
        GetProcAddress(m_sdlModule, "SDL_SetLogOutputFunction"));
    auto setMesaLog = reinterpret_cast<SetMesaLog>(
        GetProcAddress(m_mesaModule, "mesa_uwp_set_log_callback"));
    if (!attachSDL || !m_attachMesa || !m_setMesaSwapChainAttach ||
        !m_updateSDLPanelSize ||
        !setSDLLog || !setMesaLog) {
        SetError("SDL3/Mesa do not expose the expected XAML embedding API");
        return false;
    }
    if (!ResolveSDLInput()) {
        return false;
    }

    auto inspectable = reinterpret_cast<IInspectable *>(panel);
    int width = static_cast<int>(panel->ActualWidth * panel->CompositionScaleX);
    int height = static_cast<int>(panel->ActualHeight * panel->CompositionScaleY);
    int logicalWidth = static_cast<int>(panel->ActualWidth + 0.5);
    int logicalHeight = static_cast<int>(panel->ActualHeight + 0.5);
    if (logicalWidth < 1 || logicalHeight < 1 || width < 1 || height < 1) {
        SetError("SwapChainPanel does not have valid dimensions yet");
        return false;
    }
    setSDLLog(&XemuHost::SDLLog, this);
    setMesaLog(&XemuHost::MesaLog, this);
    m_renderPanel = panel;
    m_setMesaSwapChainAttach(&XemuHost::AttachMesaSwapChain, this);
    m_attachMesa(inspectable, width, height);
    if (!attachSDL(inspectable)) {
        SetError("SDL3 rejected the XAML host SwapChainPanel");
        return false;
    }
    if (!m_updateSDLPanelSize(logicalWidth, logicalHeight, width, height)) {
        SetError("SDL3 rejected the SwapChainPanel dimensions");
        return false;
    }
    WriteDiagnostic("[display] SwapChainPanel attached; SDL video initialized on the UI thread");
    return true;
}

bool XemuHost::ResolveSDLInput()
{
    m_attachVirtualJoystick = reinterpret_cast<AttachVirtualJoystick>(
        GetProcAddress(m_sdlModule, "SDL_AttachVirtualJoystick"));
    m_detachVirtualJoystick = reinterpret_cast<DetachVirtualJoystick>(
        GetProcAddress(m_sdlModule, "SDL_DetachVirtualJoystick"));
    m_openJoystick = reinterpret_cast<OpenJoystick>(
        GetProcAddress(m_sdlModule, "SDL_OpenJoystick"));
    m_closeJoystick = reinterpret_cast<CloseJoystick>(
        GetProcAddress(m_sdlModule, "SDL_CloseJoystick"));
    m_setVirtualAxis = reinterpret_cast<SetVirtualAxis>(
        GetProcAddress(m_sdlModule, "SDL_SetJoystickVirtualAxis"));
    m_setVirtualButton = reinterpret_cast<SetVirtualButton>(
        GetProcAddress(m_sdlModule, "SDL_SetJoystickVirtualButton"));
    m_setEmbeddedCursorHidden = reinterpret_cast<SetEmbeddedCursorHidden>(
        GetProcAddress(m_sdlModule, "SDL_WinRTSetEmbeddedCursorHidden"));
    if (!m_attachVirtualJoystick || !m_detachVirtualJoystick ||
        !m_openJoystick || !m_closeJoystick || !m_setVirtualAxis ||
        !m_setVirtualButton || !m_setEmbeddedCursorHidden) {
        SetError("SDL3 does not expose the virtual joystick API required on Xbox");
        return false;
    }
    WriteDiagnostic("[input] UWP Gamepad -> SDL virtual joystick bridge resolved");
    m_setEmbeddedCursorHidden(false);
    return true;
}

void XemuHost::DetachUWPGamepad()
{
    if (m_setGamepadState) {
        QemuHostGamepadState state{};
        state.size = sizeof(state);
        m_setGamepadState(0, &state);
    }
    if (m_virtualJoystick && m_closeJoystick) {
        m_closeJoystick(m_virtualJoystick);
    }
    m_virtualJoystick = nullptr;
    if (m_virtualJoystickId && m_detachVirtualJoystick) {
        m_detachVirtualJoystick(m_virtualJoystickId);
    }
    m_virtualJoystickId = 0;
    m_uwpGamepad = nullptr;
    m_gamepadErrorLogged = false;
}

void XemuHost::UpdateUWPGamepad()
{
    auto gamepads = Gamepad::Gamepads;
    if (!gamepads || gamepads->Size == 0) {
        if (m_virtualJoystickId) {
            WriteDiagnostic("[input] Xbox controller disconnected");
            DetachUWPGamepad();
        }
        return;
    }

    Gamepad^ gamepad = gamepads->GetAt(0);
    if (gamepad != m_uwpGamepad || !m_virtualJoystick) {
        DetachUWPGamepad();
        SDL_VirtualJoystickDesc desc{};
        desc.version = sizeof(desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.vendor_id = 0x045e;
        desc.product_id = 0x02ff;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1u;
        desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1u;
        desc.name = "Xbox Gamepad (UWP)";
        desc.userdata = this;
        m_virtualJoystickId = m_attachVirtualJoystick(&desc);
        if (!m_virtualJoystickId) {
            WriteDiagnostic("[input] Failed to attach SDL virtual joystick");
            return;
        }
        m_virtualJoystick = m_openJoystick(m_virtualJoystickId);
        if (!m_virtualJoystick) {
            WriteDiagnostic("[input] Failed to open SDL virtual joystick");
            m_detachVirtualJoystick(m_virtualJoystickId);
            m_virtualJoystickId = 0;
            return;
        }
        m_uwpGamepad = gamepad;
        WriteDiagnostic("[input] Xbox Gamepad (UWP) connected to SDL; WinRT devices=" +
                        std::to_string(gamepads->Size));
    }

    unsigned int pressedButtons = 0;
    double leftTrigger = 0.0;
    double rightTrigger = 0.0;
    double leftX = 0.0;
    double leftY = 0.0;
    double rightX = 0.0;
    double rightY = 0.0;
    uint64_t latestTimestamp = 0;
    auto strongest = [](double current, double candidate) {
        return fabs(candidate) > fabs(current) ? candidate : current;
    };
    for (unsigned int i = 0; i < gamepads->Size; ++i) {
        GamepadReading candidate = gamepads->GetAt(i)->GetCurrentReading();
        pressedButtons |= static_cast<unsigned int>(candidate.Buttons);
        leftTrigger = (std::max)(leftTrigger, candidate.LeftTrigger);
        rightTrigger = (std::max)(rightTrigger, candidate.RightTrigger);
        leftX = strongest(leftX, candidate.LeftThumbstickX);
        leftY = strongest(leftY, candidate.LeftThumbstickY);
        rightX = strongest(rightX, candidate.RightThumbstickX);
        rightY = strongest(rightY, candidate.RightThumbstickY);
        latestTimestamp = (std::max)(latestTimestamp, candidate.Timestamp);
    }
    if (latestTimestamp != m_lastGamepadTimestamp &&
        m_gamepadChangeLogs < 32) {
        const bool active = pressedButtons != 0 || leftTrigger > 0.01 ||
            rightTrigger > 0.01 || fabs(leftX) > 0.05 ||
            fabs(leftY) > 0.05 || fabs(rightX) > 0.05 ||
            fabs(rightY) > 0.05;
        if (active) {
            std::ostringstream message;
            message << "[input] GamepadReading buttons=0x" << std::hex
                    << pressedButtons << std::dec << " triggers="
                    << leftTrigger << "," << rightTrigger << " sticks="
                    << leftX << "," << leftY << "," << rightX << ","
                    << rightY << " devices=" << gamepads->Size;
            WriteDiagnostic(message.str());
            ++m_gamepadChangeLogs;
        }
        m_lastGamepadTimestamp = latestTimestamp;
    }
    struct ButtonMap { GamepadButtons source; int target; };
    static const ButtonMap buttons[] = {
        { GamepadButtons::A, SDL_GAMEPAD_BUTTON_SOUTH },
        { GamepadButtons::B, SDL_GAMEPAD_BUTTON_EAST },
        { GamepadButtons::X, SDL_GAMEPAD_BUTTON_WEST },
        { GamepadButtons::Y, SDL_GAMEPAD_BUTTON_NORTH },
        { GamepadButtons::View, SDL_GAMEPAD_BUTTON_BACK },
        { GamepadButtons::Menu, SDL_GAMEPAD_BUTTON_START },
        { GamepadButtons::LeftThumbstick, SDL_GAMEPAD_BUTTON_LEFT_STICK },
        { GamepadButtons::RightThumbstick, SDL_GAMEPAD_BUTTON_RIGHT_STICK },
        { GamepadButtons::LeftShoulder, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER },
        { GamepadButtons::RightShoulder, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER },
        { GamepadButtons::DPadUp, SDL_GAMEPAD_BUTTON_DPAD_UP },
        { GamepadButtons::DPadDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN },
        { GamepadButtons::DPadLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT },
        { GamepadButtons::DPadRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT },
    };
    for (const auto& button : buttons) {
        m_setVirtualButton(m_virtualJoystick, button.target,
            (pressedButtons & static_cast<unsigned int>(button.source)) != 0);
    }
    auto stick = [](double value) -> int16_t {
        double scaled = value < 0.0 ? value * 32768.0 : value * 32767.0;
        return static_cast<int16_t>(scaled);
    };
    auto trigger = [](double value) -> int16_t {
        return static_cast<int16_t>(value * 65535.0 - 32768.0);
    };
    m_setVirtualAxis(m_virtualJoystick, SDL_GAMEPAD_AXIS_LEFTX,
                     stick(leftX));
    m_setVirtualAxis(m_virtualJoystick, SDL_GAMEPAD_AXIS_LEFTY,
                     stick(-leftY));
    m_setVirtualAxis(m_virtualJoystick, SDL_GAMEPAD_AXIS_RIGHTX,
                     stick(rightX));
    m_setVirtualAxis(m_virtualJoystick, SDL_GAMEPAD_AXIS_RIGHTY,
                     stick(-rightY));
    m_setVirtualAxis(m_virtualJoystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
                     trigger(leftTrigger));
    m_setVirtualAxis(m_virtualJoystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
                     trigger(rightTrigger));

    QemuHostGamepadState hostState{};
    hostState.size = sizeof(hostState);
    hostState.connected = true;
    const struct HostButtonMap { GamepadButtons source; uint32_t target; }
        hostButtons[] = {
            { GamepadButtons::A, QEMU_HOST_GAMEPAD_A },
            { GamepadButtons::B, QEMU_HOST_GAMEPAD_B },
            { GamepadButtons::X, QEMU_HOST_GAMEPAD_X },
            { GamepadButtons::Y, QEMU_HOST_GAMEPAD_Y },
            { GamepadButtons::DPadLeft, QEMU_HOST_GAMEPAD_DPAD_LEFT },
            { GamepadButtons::DPadUp, QEMU_HOST_GAMEPAD_DPAD_UP },
            { GamepadButtons::DPadRight, QEMU_HOST_GAMEPAD_DPAD_RIGHT },
            { GamepadButtons::DPadDown, QEMU_HOST_GAMEPAD_DPAD_DOWN },
            { GamepadButtons::View, QEMU_HOST_GAMEPAD_BACK },
            { GamepadButtons::Menu, QEMU_HOST_GAMEPAD_START },
            { GamepadButtons::LeftShoulder,
              QEMU_HOST_GAMEPAD_LEFT_SHOULDER },
            { GamepadButtons::RightShoulder,
              QEMU_HOST_GAMEPAD_RIGHT_SHOULDER },
            { GamepadButtons::LeftThumbstick,
              QEMU_HOST_GAMEPAD_LEFT_STICK },
            { GamepadButtons::RightThumbstick,
              QEMU_HOST_GAMEPAD_RIGHT_STICK },
        };
    for (const auto& button : hostButtons) {
        if (pressedButtons & static_cast<unsigned int>(button.source)) {
            hostState.buttons |= button.target;
        }
    }
    auto hostTrigger = [](double value) -> int16_t {
        return static_cast<int16_t>(value * 32767.0);
    };
    hostState.left_trigger = hostTrigger(leftTrigger);
    hostState.right_trigger = hostTrigger(rightTrigger);
    hostState.left_x = stick(leftX);
    hostState.left_y = stick(leftY);
    hostState.right_x = stick(rightX);
    hostState.right_y = stick(rightY);
    if (m_setGamepadState(0, &hostState) != 0 && !m_gamepadErrorLogged) {
        WriteDiagnostic("[input] Failed to send direct state to XID");
        m_gamepadErrorLogged = true;
    }

}

long __cdecl XemuHost::AttachMesaSwapChain(void* opaque, void* swapchain)
{
    auto host = static_cast<XemuHost*>(opaque);
    auto panel = host ? host->m_renderPanel : nullptr;
    auto nativeSwapChain = static_cast<IDXGISwapChain*>(swapchain);
    if (!panel || !nativeSwapChain) {
        return E_POINTER;
    }

    Microsoft::WRL::ComPtr<IDXGISwapChain> retainedSwapChain = nativeSwapChain;
    auto attach = [host, panel, retainedSwapChain]() -> HRESULT {
        Microsoft::WRL::ComPtr<ISwapChainPanelNative> panelNative;
        HRESULT result = reinterpret_cast<IUnknown*>(panel)->QueryInterface(
            IID_PPV_ARGS(&panelNative));
        if (FAILED(result)) {
            return result;
        }

        Microsoft::WRL::ComPtr<IDXGISwapChain2> swapChain2;
        result = retainedSwapChain.As(&swapChain2);
        if (FAILED(result)) {
            return result;
        }

        DXGI_MATRIX_3X2_F inverseScale{};
        inverseScale._11 = 1.0f / panel->CompositionScaleX;
        inverseScale._22 = 1.0f / panel->CompositionScaleY;
        result = swapChain2->SetMatrixTransform(&inverseScale);
        if (FAILED(result)) {
            return result;
        }

        result = panelNative->SetSwapChain(retainedSwapChain.Get());
        if (SUCCEEDED(result)) {
            host->m_swapChain = swapChain2;
        }
        return result;
    };

    if (panel->Dispatcher->HasThreadAccess) {
        return attach();
    }

    HRESULT result = E_FAIL;
    try {
        auto operation = panel->Dispatcher->RunAsync(
            Windows::UI::Core::CoreDispatcherPriority::High,
            ref new Windows::UI::Core::DispatchedHandler([&result, attach]() {
                result = attach();
            }));
        concurrency::create_task(operation).wait();
    } catch (Platform::Exception^ exception) {
        result = exception->HResult;
    }
    return result;
}

bool XemuHost::UpdateRenderPanelSize(
    Windows::UI::Xaml::Controls::SwapChainPanel^ panel)
{
    if (!panel || !m_attachMesa || !m_updateSDLPanelSize) {
        return false;
    }

    int logicalWidth = static_cast<int>(panel->ActualWidth + 0.5);
    int logicalHeight = static_cast<int>(panel->ActualHeight + 0.5);
    int pixelWidth = static_cast<int>(
        panel->ActualWidth * panel->CompositionScaleX + 0.5);
    int pixelHeight = static_cast<int>(
        panel->ActualHeight * panel->CompositionScaleY + 0.5);
    if (logicalWidth < 1 || logicalHeight < 1 ||
        pixelWidth < 1 || pixelHeight < 1) {
        return false;
    }

    m_attachMesa(reinterpret_cast<IInspectable *>(panel),
                 pixelWidth, pixelHeight);
    if (!m_updateSDLPanelSize(logicalWidth, logicalHeight,
                              pixelWidth, pixelHeight)) {
        return false;
    }

    if (m_swapChain) {
        DXGI_MATRIX_3X2_F inverseScale{};
        inverseScale._11 = 1.0f / panel->CompositionScaleX;
        inverseScale._22 = 1.0f / panel->CompositionScaleY;
        HRESULT result = m_swapChain->SetMatrixTransform(&inverseScale);
        if (FAILED(result)) {
            std::ostringstream message;
            message << "Failed to remove XAML scaling from the swapchain (0x"
                    << std::hex << static_cast<unsigned long>(result) << ")";
            SetError(message.str());
            return false;
        }
    }
    return true;
}

void __cdecl XemuHost::MesaLog(void* opaque, const char* message)
{
    auto host = static_cast<XemuHost*>(opaque);
    if (host && message) {
        std::string text(message);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        host->WriteDiagnostic(std::string("[mesa] ") + text);
    }
}

void __cdecl XemuHost::SDLLog(void* opaque, int, int, const char* message)
{
    auto host = static_cast<XemuHost*>(opaque);
    if (host && message) {
        host->WriteDiagnostic(std::string("[sdl] ") + message);
    }
}

template<typename T>
bool XemuHost::Resolve(T& target, const char* name)
{
    target = reinterpret_cast<T>(GetProcAddress(m_module, name));
    if (!target) {
        SetError(std::string("Missing API in qemu-system-i386.dll: ") + name);
        return false;
    }
    WriteDiagnostic(std::string("[loader] API resolved: ") + name);
    return true;
}

bool XemuHost::Load()
{
    if (m_module) {
        return true;
    }
    WriteDiagnostic("[loader] Loading qemu-system-i386.dll");
    m_module = LoadPackagedLibrary(L"qemu-system-i386.dll", 0);
    if (!m_module) {
        DWORD error = GetLastError();
        std::ostringstream message;
        message << "Failed to load qemu-system-i386.dll from the package (Win32 error "
                << error << ")";
        SetError(message.str());
        return false;
    }
    WriteDiagnostic("[loader] qemu-system-i386.dll loaded");

    bool ok = Resolve(m_getApiVersion, "qemu_host_get_api_version") &&
              Resolve(m_init, "qemu_host_init") &&
              Resolve(m_start, "qemu_host_start") &&
              Resolve(m_renderFrame, "qemu_host_render_frame") &&
              Resolve(m_step, "qemu_host_main_loop_step") &&
              Resolve(m_isHostRunning, "qemu_host_is_running") &&
              Resolve(m_requestStop, "qemu_host_request_stop") &&
              Resolve(m_pause, "qemu_host_pause") &&
              Resolve(m_resume, "qemu_host_resume") &&
              Resolve(m_reset, "qemu_host_reset") &&
              Resolve(m_shutdown, "qemu_host_request_shutdown") &&
              Resolve(m_join, "qemu_host_join") &&
              Resolve(m_cleanup, "qemu_host_cleanup") &&
              Resolve(m_registerLog, "qemu_host_register_log_callback") &&
              Resolve(m_setLogFile, "qemu_host_set_log_file") &&
              Resolve(m_setGamepadState, "qemu_host_set_gamepad_state") &&
              Resolve(m_registerBrokeredStorage, "qemu_host_register_brokered_storage_callbacks") &&
              Resolve(m_mountFile, "qemu_host_mount_brokered_file") &&
              Resolve(m_mountFolder, "qemu_host_mount_brokered_folder");
    if (!ok || (m_getApiVersion() >> 16) != QEMU_HOST_API_VERSION_MAJOR) {
        SetError("Incompatible xemu embedding API version");
        return false;
    }
    WriteDiagnostic("[loader] Embedding API is compatible");
    m_registerLog(&XemuHost::Log, this);
    WriteDiagnostic("[loader] xemu log callback registered");

    QemuHostBrokeredStorageCallbacks storage{};
    storage.size = sizeof(storage);
    storage.version = QEMU_HOST_BROKERED_STORAGE_CALLBACKS_VERSION;
    storage.retain = &XemuHost::RetainBrokeredObject;
    storage.release = &XemuHost::ReleaseBrokeredObject;
    storage.open_file = &XemuHost::OpenBrokeredFile;
    storage.open_at = &XemuHost::OpenBrokeredPath;
    storage.read = &XemuHost::ReadBrokeredFile;
    storage.write = &XemuHost::WriteBrokeredFile;
    storage.seek = &XemuHost::SeekBrokeredFile;
    storage.close = &XemuHost::CloseBrokeredFile;
    storage.stat_file = &XemuHost::StatBrokeredFile;
    storage.flush = &XemuHost::FlushBrokeredFile;
    storage.readdir = &XemuHost::ReadBrokeredDirectory;
    storage.truncate = &XemuHost::TruncateBrokeredFile;
    int storageResult = m_registerBrokeredStorage(&storage, this);
    WriteDiagnostic("[storage] Brokered callback registration returned " +
                    std::to_string(storageResult));
    if (storageResult) {
        SetError("Failed to register brokered storage (error " +
                 std::to_string(storageResult) + ")");
        return false;
    }
    return true;
}

bool XemuHost::Start(const std::vector<std::string>& arguments)
{
    if (m_running.load() || !Load()) {
        return m_running.load();
    }
    WriteDiagnostic("[lifecycle] Request to start xemu");
    if (m_setEmbeddedCursorHidden) {
        m_setEmbeddedCursorHidden(true);
    }
    m_stop.store(false);
    m_thread = std::thread(&XemuHost::Run, this, arguments);
    return true;
}

void XemuHost::Run(std::vector<std::string> arguments)
{
    WriteDiagnostic("[lifecycle] Initialization thread started");
    HRESULT apartmentResult = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(apartmentResult) && apartmentResult != RPC_E_CHANGED_MODE) {
        std::ostringstream message;
        message << "Failed to initialize the WinRT apartment for the xemu thread (0x"
                << std::hex << static_cast<unsigned long>(apartmentResult)
                << ")";
        SetError(message.str());
        return;
    }
    bool uninitializeApartment = SUCCEEDED(apartmentResult);
    WriteDiagnostic("[lifecycle] WinRT MTA apartment initialized");
    if (arguments.empty()) {
        auto configPath = ApplicationData::Current->LocalFolder->Path +
                          L"\\xemu.toml";
        std::wstring wide(configPath->Data());
        int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1,
                                       nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &utf8[0], size,
                            nullptr, nullptr);
        utf8.pop_back();
        arguments = { "xemu", "-config_path", utf8 };
    } else {
        arguments.insert(arguments.begin(), "xemu");
    }
    std::vector<char*> argv;
    for (auto& arg : arguments) {
        argv.push_back(&arg[0]);
    }
    argv.push_back(nullptr);

    WriteDiagnostic("[lifecycle] Calling qemu_host_init");
    int rc = m_init(static_cast<int>(arguments.size()), argv.data());
    WriteDiagnostic("[lifecycle] qemu_host_init returned " + std::to_string(rc));
    if (!rc) {
        WriteDiagnostic("[lifecycle] Calling qemu_host_start");
        rc = m_start();
        WriteDiagnostic("[lifecycle] qemu_host_start returned " + std::to_string(rc));
    }
    m_running.store(rc == 0);
    if (!rc) {
        int status = 0;
        WriteDiagnostic("[lifecycle] Waiting for the xemu main loop");
        int joinResult = m_join(&status);
        WriteDiagnostic("[lifecycle] qemu_host_join returned " +
                        std::to_string(joinResult) + ", status " +
                        std::to_string(status));
        if (!rc) {
            rc = joinResult;
        }
    }
    m_running.store(false);
    WriteDiagnostic("[lifecycle] Calling qemu_host_cleanup");
    m_cleanup();
    if (rc) {
        SetError("The embedded xemu loop ended with error " +
                 std::to_string(rc));
    }
    if (uninitializeApartment) {
        RoUninitialize();
    }
}

void XemuHost::Stop()
{
    WriteDiagnostic("[lifecycle] Request to stop xemu");
    m_stop.store(true);
    if (m_running.load() && m_requestStop) {
        m_requestStop();
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_setEmbeddedCursorHidden) {
        m_setEmbeddedCursorHidden(false);
    }
}

void XemuHost::Pause() { if (m_running.load()) m_pause(); }
void XemuHost::Resume() { if (m_running.load()) m_resume(); }
void XemuHost::Reset() { if (m_running.load()) m_reset(); }
void XemuHost::Shutdown() { if (m_running.load()) m_shutdown(); }

bool XemuHost::RenderFrame()
{
    if (!m_running.load() || !m_renderFrame || !m_isHostRunning()) {
        return false;
    }
    try {
        UpdateUWPGamepad();
    } catch (Platform::Exception^ exception) {
        if (!m_gamepadErrorLogged) {
            std::ostringstream message;
            message << "[input] UWP Gamepad bridge failed (0x" << std::hex
                    << static_cast<unsigned long>(exception->HResult) << ")";
            WriteDiagnostic(message.str());
            m_gamepadErrorLogged = true;
        }
    }
    bool firstFrame = !m_firstFrameLogged.exchange(true);
    if (firstFrame) {
        WriteDiagnostic("[display] First OpenGL frame started on the XAML thread");
    }
    int rc = m_renderFrame();
    if (firstFrame) {
        WriteDiagnostic(rc == 0 ?
            "[display] First OpenGL frame presented" :
            "[display] First OpenGL frame failed: " + std::to_string(rc));
    }
    return rc == 0;
}

bool XemuHost::MountFile(const std::string& virtualPath, StorageFile^ file,
                         IRandomAccessStream^ stream)
{
    if (!file || !stream || !Load()) {
        SetError("Failed to prepare " + virtualPath + " for mounting");
        return false;
    }
    int rc = m_mountFile(virtualPath.c_str(), reinterpret_cast<IInspectable*>(file),
                         reinterpret_cast<IInspectable*>(stream));
    WriteDiagnostic("[storage] Mount " + virtualPath + " returned " +
                    std::to_string(rc));
    if (rc) {
        SetError("Failed to mount " + virtualPath + " (error " +
                 std::to_string(rc) + ")");
    }
    return rc == 0;
}

bool XemuHost::MountFolder(const std::string& virtualPath,
                           StorageFolder^ folder)
{
    return m_mountFolder && folder &&
        m_mountFolder(virtualPath.c_str(),
                      reinterpret_cast<IInspectable*>(folder)) == 0;
}

void XemuHost::SetError(const std::string& error)
{
    WriteDiagnostic("[error] " + error);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_error = error;
}

std::string XemuHost::LastError() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_error;
}

void XemuHost::WriteDiagnostic(const std::string& message)
{
    std::lock_guard<std::mutex> lock(m_logMutex);
    std::string line = message + "\r\n";
    OutputDebugStringA(line.c_str());
    CREATEFILE2_EXTENDED_PARAMETERS params{};
    params.dwSize = sizeof(params);
    params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    HANDLE file = CreateFile2(m_logPath.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS,
                              &params);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written,
                  nullptr);
        FlushFileBuffers(file);
        CloseHandle(file);
    }
}

void XemuHost::RetainBrokeredObject(void*, void* object)
{
    if (object) {
        reinterpret_cast<IInspectable*>(object)->AddRef();
    }
}

void XemuHost::ReleaseBrokeredObject(void*, void* object)
{
    if (object) {
        reinterpret_cast<IInspectable*>(object)->Release();
    }
}

int XemuHost::OpenBrokeredFile(void* opaque, void* storageFile,
                               void* randomAccessStream,
                               int, int64_t* handle)
{
    if (!storageFile || !randomAccessStream || !handle) {
        return -EINVAL;
    }
    try {
        /* QEMU may open the same image more than once while probing its
           format and while creating the block backend. IRandomAccessStream
           keeps its cursor in the stream object, so sharing the mounted
           instance corrupts otherwise independent seek/read sequences. */
        auto source = reinterpret_cast<IRandomAccessStream^>(randomAccessStream);
        auto file = reinterpret_cast<StorageFile^>(storageFile);
        std::wstring wideName(file->Name->Data());
        int utf8Size = WideCharToMultiByte(CP_UTF8, 0, wideName.c_str(), -1,
                                           nullptr, 0, nullptr, nullptr);
        std::string name(static_cast<size_t>(utf8Size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wideName.c_str(), -1, &name[0],
                            utf8Size, nullptr, nullptr);
        name.pop_back();
        auto stream = source->CloneStream();
        stream->Seek(0);
        auto brokered = new BrokeredFileStream(stream, name);
        *handle = reinterpret_cast<int64_t>(brokered);
        auto self = static_cast<XemuHost*>(opaque);
        if (self) {
            self->WriteDiagnostic("[storage] Handle opened: " + name +
                                  ", size " +
                                  std::to_string(stream->Size) + " bytes");
        }
        return 0;
    } catch (...) {
        return -EIO;
    }
}

int XemuHost::OpenBrokeredPath(void*, void*, const char*, int, int, int64_t*)
{
    return -ENOSYS;
}

int64_t XemuHost::ReadBrokeredFile(void* opaque, int64_t handle, void* buffer,
                                   size_t size)
{
    if (!handle || (!buffer && size)) {
        return -EINVAL;
    }
    try {
        auto self = static_cast<XemuHost*>(opaque);
        static std::atomic<uint32_t> readSequence{ 0 };
        uint32_t sequence = readSequence.fetch_add(1);
        auto brokered = reinterpret_cast<BrokeredFileStream*>(handle);
        uint64_t position = brokered->stream->Position;
        unsigned int count = static_cast<unsigned int>((std::min)(
            size, static_cast<size_t>((std::numeric_limits<unsigned int>::max)())));
        if (self && sequence < 32) {
            self->WriteDiagnostic("[storage] Brokered read #" +
                                  std::to_string(sequence) + " started: " +
                                  brokered->name + " @" +
                                  std::to_string(position) + ", " +
                                  std::to_string(count) + " bytes");
        }
        auto reader = ref new DataReader(brokered->stream);
        reader->InputStreamOptions = InputStreamOptions::Partial;
        unsigned int loaded = create_task(reader->LoadAsync(count)).get();
        if (loaded) {
            reader->ReadBytes(Platform::ArrayReference<unsigned char>(
                static_cast<unsigned char*>(buffer), loaded));
        }
        reader->DetachStream();
        if (self && sequence < 32) {
            self->WriteDiagnostic("[storage] Brokered read #" +
                                  std::to_string(sequence) + " completed: " +
                                  std::to_string(loaded) + " bytes");
        }
        return loaded;
    } catch (...) {
        auto self = static_cast<XemuHost*>(opaque);
        if (self) {
            self->WriteDiagnostic("[storage] Exception during brokered read");
        }
        return -EIO;
    }
}

int64_t XemuHost::WriteBrokeredFile(void*, int64_t handle,
                                    const void* buffer, size_t size)
{
    if (!handle || (!buffer && size)) {
        return -EINVAL;
    }
    try {
        auto brokered = reinterpret_cast<BrokeredFileStream*>(handle);
        if (!brokered->stream->CanWrite) {
            return -EROFS;
        }
        unsigned int count = static_cast<unsigned int>((std::min)(
            size, static_cast<size_t>((std::numeric_limits<unsigned int>::max)())));
        auto writer = ref new DataWriter(brokered->stream);
        writer->WriteBytes(Platform::ArrayReference<unsigned char>(
            const_cast<unsigned char*>(static_cast<const unsigned char*>(buffer)),
            count));
        unsigned int written = create_task(writer->StoreAsync()).get();
        writer->DetachStream();
        return written;
    } catch (...) {
        return -EIO;
    }
}

int64_t XemuHost::SeekBrokeredFile(void*, int64_t handle, int64_t offset,
                                   int whence)
{
    if (!handle) {
        return -EINVAL;
    }
    try {
        auto stream = reinterpret_cast<BrokeredFileStream*>(handle)->stream;
        int64_t base = whence == SEEK_SET ? 0 :
                       whence == SEEK_CUR ? static_cast<int64_t>(stream->Position) :
                       whence == SEEK_END ? static_cast<int64_t>(stream->Size) : -1;
        if (base < 0 || offset < -base) {
            return -EINVAL;
        }
        uint64_t position = static_cast<uint64_t>(base + offset);
        stream->Seek(position);
        return static_cast<int64_t>(position);
    } catch (...) {
        return -EIO;
    }
}

int XemuHost::CloseBrokeredFile(void*, int64_t handle)
{
    if (!handle) {
        return -EINVAL;
    }
    delete reinterpret_cast<BrokeredFileStream*>(handle);
    return 0;
}

int XemuHost::StatBrokeredFile(void*, void* storageFile,
                               void* randomAccessStream,
                               QemuHostStorageStat* stat)
{
    if (!storageFile || !randomAccessStream || !stat) {
        return -EINVAL;
    }
    try {
        auto file = reinterpret_cast<StorageFile^>(storageFile);
        auto properties = create_task(file->GetBasicPropertiesAsync()).get();
        memset(stat, 0, sizeof(*stat));
        stat->size = properties->Size;
        stat->allocated_size = properties->Size;
        stat->mode = 0100666;
        stat->type = 1;
        return 0;
    } catch (...) {
        return -EIO;
    }
}

int XemuHost::FlushBrokeredFile(void*, int64_t handle)
{
    if (!handle) {
        return -EINVAL;
    }
    try {
        auto stream = reinterpret_cast<BrokeredFileStream*>(handle)->stream;
        return create_task(stream->FlushAsync()).get() ? 0 : -EIO;
    } catch (...) {
        return -EIO;
    }
}

int XemuHost::ReadBrokeredDirectory(void*, int64_t, char*, size_t,
                                    QemuHostStorageStat*)
{
    return -ENOSYS;
}

int XemuHost::TruncateBrokeredFile(void*, int64_t handle, uint64_t size)
{
    if (!handle) {
        return -EINVAL;
    }
    try {
        auto stream = reinterpret_cast<BrokeredFileStream*>(handle)->stream;
        if (!stream->CanWrite) {
            return -EROFS;
        }
        stream->Size = size;
        return 0;
    } catch (...) {
        return -EIO;
    }
}

void __cdecl XemuHost::Log(void* opaque, QemuHostLogLevel level,
                           const char* message)
{
    if (message) {
        static const char* levels[] = { "ERROR", "WARN", "INFO", "DEBUG" };
        int index = level >= QEMU_HOST_LOG_ERROR && level <= QEMU_HOST_LOG_DEBUG ?
                    static_cast<int>(level) : 0;
        static_cast<XemuHost*>(opaque)->WriteDiagnostic(
            std::string("[xemu/") + levels[index] + "] " + message);
    }
}
