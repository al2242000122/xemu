#include "pch.h"
#include "XemuHost.h"

#include <robuffer.h>
#include <roapi.h>
#include <windows.storage.h>
#include <windows.ui.xaml.media.dxinterop.h>

#include <cerrno>
#include <limits>
#include <sstream>

using namespace UWP_Port;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;
using namespace concurrency;

namespace {
struct BrokeredFileStream {
    IRandomAccessStream^ stream;
};
}

XemuHost::XemuHost()
    : m_module(nullptr), m_sdlModule(nullptr), m_openGLModule(nullptr),
      m_mesaModule(nullptr),
      m_running(false), m_stop(false), m_firstFrameLogged(false),
      m_attachMesa(nullptr), m_setMesaSwapChainAttach(nullptr),
      m_updateSDLPanelSize(nullptr), m_renderPanel(nullptr),
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
    WriteDiagnostic("[host] Nova sessao de diagnostico UWP (desktop debug ativo)");
}

XemuHost::~XemuHost()
{
    WriteDiagnostic("[host] Encerrando XemuHost");
    Stop();
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
        SetError("SwapChainPanel do xemu nao foi fornecido");
        return false;
    }

    WriteDiagnostic("[display] Anexando SwapChainPanel ao SDL3 e ao Mesa");
    m_sdlModule = LoadPackagedLibrary(L"SDL3.dll", 0);
    m_mesaModule = LoadPackagedLibrary(L"gallium_wgl.dll", 0);
    m_openGLModule = LoadPackagedLibrary(L"opengl32.dll", 0);
    if (!m_sdlModule || !m_mesaModule || !m_openGLModule) {
        SetError("Falha ao carregar SDL3.dll, opengl32.dll ou gallium_wgl.dll para preparar o renderer");
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
        SetError("SDL3/Mesa nao expoem a API de embedding XAML esperada");
        return false;
    }

    auto inspectable = reinterpret_cast<IInspectable *>(panel);
    int width = static_cast<int>(panel->ActualWidth * panel->CompositionScaleX);
    int height = static_cast<int>(panel->ActualHeight * panel->CompositionScaleY);
    int logicalWidth = static_cast<int>(panel->ActualWidth + 0.5);
    int logicalHeight = static_cast<int>(panel->ActualHeight + 0.5);
    if (logicalWidth < 1 || logicalHeight < 1 || width < 1 || height < 1) {
        SetError("SwapChainPanel ainda nao possui dimensoes validas");
        return false;
    }
    setSDLLog(&XemuHost::SDLLog, this);
    setMesaLog(&XemuHost::MesaLog, this);
    m_renderPanel = panel;
    m_setMesaSwapChainAttach(&XemuHost::AttachMesaSwapChain, this);
    m_attachMesa(inspectable, width, height);
    if (!attachSDL(inspectable)) {
        SetError("SDL3 rejeitou o SwapChainPanel do host XAML");
        return false;
    }
    if (!m_updateSDLPanelSize(logicalWidth, logicalHeight, width, height)) {
        SetError("SDL3 rejeitou as dimensoes do SwapChainPanel");
        return false;
    }
    WriteDiagnostic("[display] SwapChainPanel anexado; SDL video inicializado no thread da UI");
    return true;
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
    auto attach = [panel, retainedSwapChain]() -> HRESULT {
        Microsoft::WRL::ComPtr<ISwapChainPanelNative> panelNative;
        HRESULT result = reinterpret_cast<IUnknown*>(panel)->QueryInterface(
            IID_PPV_ARGS(&panelNative));
        return SUCCEEDED(result) ? panelNative->SetSwapChain(retainedSwapChain.Get())
                                 : result;
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
    return m_updateSDLPanelSize(logicalWidth, logicalHeight,
                                pixelWidth, pixelHeight);
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
        SetError(std::string("API ausente em qemu-system-i386.dll: ") + name);
        return false;
    }
    WriteDiagnostic(std::string("[loader] API resolvida: ") + name);
    return true;
}

bool XemuHost::Load()
{
    if (m_module) {
        return true;
    }
    WriteDiagnostic("[loader] Carregando qemu-system-i386.dll");
    m_module = LoadPackagedLibrary(L"qemu-system-i386.dll", 0);
    if (!m_module) {
        DWORD error = GetLastError();
        std::ostringstream message;
        message << "Falha ao carregar qemu-system-i386.dll do pacote (erro Win32 "
                << error << ")";
        SetError(message.str());
        return false;
    }
    WriteDiagnostic("[loader] qemu-system-i386.dll carregada");

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
              Resolve(m_registerBrokeredStorage, "qemu_host_register_brokered_storage_callbacks") &&
              Resolve(m_mountFile, "qemu_host_mount_brokered_file") &&
              Resolve(m_mountFolder, "qemu_host_mount_brokered_folder");
    if (!ok || (m_getApiVersion() >> 16) != QEMU_HOST_API_VERSION_MAJOR) {
        SetError("Versao incompativel da API de embedding do xemu");
        return false;
    }
    WriteDiagnostic("[loader] API de embedding compativel");
    m_registerLog(&XemuHost::Log, this);
    WriteDiagnostic("[loader] Callback de log do xemu registrado");

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
    WriteDiagnostic("[storage] Registro dos callbacks brokered retornou " +
                    std::to_string(storageResult));
    if (storageResult) {
        SetError("Falha ao registrar armazenamento brokered (erro " +
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
    WriteDiagnostic("[lifecycle] Solicitacao para iniciar xemu");
    m_stop.store(false);
    m_thread = std::thread(&XemuHost::Run, this, arguments);
    return true;
}

void XemuHost::Run(std::vector<std::string> arguments)
{
    WriteDiagnostic("[lifecycle] Thread de inicializacao iniciada");
    HRESULT apartmentResult = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(apartmentResult) && apartmentResult != RPC_E_CHANGED_MODE) {
        std::ostringstream message;
        message << "Falha ao inicializar apartment WinRT da thread do xemu (0x"
                << std::hex << static_cast<unsigned long>(apartmentResult)
                << ")";
        SetError(message.str());
        return;
    }
    bool uninitializeApartment = SUCCEEDED(apartmentResult);
    WriteDiagnostic("[lifecycle] Apartment WinRT MTA inicializado");
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

    WriteDiagnostic("[lifecycle] Chamando qemu_host_init");
    int rc = m_init(static_cast<int>(arguments.size()), argv.data());
    WriteDiagnostic("[lifecycle] qemu_host_init retornou " + std::to_string(rc));
    if (!rc) {
        WriteDiagnostic("[lifecycle] Chamando qemu_host_start");
        rc = m_start();
        WriteDiagnostic("[lifecycle] qemu_host_start retornou " + std::to_string(rc));
    }
    m_running.store(rc == 0);
    if (!rc) {
        int status = 0;
        WriteDiagnostic("[lifecycle] Aguardando o loop principal do xemu");
        int joinResult = m_join(&status);
        WriteDiagnostic("[lifecycle] qemu_host_join retornou " +
                        std::to_string(joinResult) + ", status " +
                        std::to_string(status));
        if (!rc) {
            rc = joinResult;
        }
    }
    m_running.store(false);
    WriteDiagnostic("[lifecycle] Chamando qemu_host_cleanup");
    m_cleanup();
    if (rc) {
        SetError("O loop incorporado do xemu terminou com erro " +
                 std::to_string(rc));
    }
    if (uninitializeApartment) {
        RoUninitialize();
    }
}

void XemuHost::Stop()
{
    WriteDiagnostic("[lifecycle] Solicitacao para parar xemu");
    m_stop.store(true);
    if (m_running.load() && m_requestStop) {
        m_requestStop();
    }
    if (m_thread.joinable()) {
        m_thread.join();
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
    bool firstFrame = !m_firstFrameLogged.exchange(true);
    if (firstFrame) {
        WriteDiagnostic("[display] Primeiro frame OpenGL no thread XAML iniciado");
    }
    int rc = m_renderFrame();
    if (firstFrame) {
        WriteDiagnostic(rc == 0 ?
            "[display] Primeiro frame OpenGL apresentado" :
            "[display] Falha no primeiro frame OpenGL: " + std::to_string(rc));
    }
    return rc == 0;
}

bool XemuHost::MountFile(const std::string& virtualPath, StorageFile^ file,
                         IRandomAccessStream^ stream)
{
    if (!file || !stream || !Load()) {
        SetError("Falha ao preparar " + virtualPath + " para montagem");
        return false;
    }
    int rc = m_mountFile(virtualPath.c_str(), reinterpret_cast<IInspectable*>(file),
                         reinterpret_cast<IInspectable*>(stream));
    WriteDiagnostic("[storage] Montagem " + virtualPath + " retornou " +
                    std::to_string(rc));
    if (rc) {
        SetError("Falha ao montar " + virtualPath + " (erro " +
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
    WriteDiagnostic("[erro] " + error);
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

int XemuHost::OpenBrokeredFile(void*, void*, void* randomAccessStream,
                               int, int64_t* handle)
{
    if (!randomAccessStream || !handle) {
        return -EINVAL;
    }
    try {
        auto stream = reinterpret_cast<IRandomAccessStream^>(randomAccessStream);
        auto brokered = new BrokeredFileStream{ stream };
        *handle = reinterpret_cast<int64_t>(brokered);
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
        unsigned int count = static_cast<unsigned int>((std::min)(
            size, static_cast<size_t>((std::numeric_limits<unsigned int>::max)())));
        if (self && sequence < 32) {
            self->WriteDiagnostic("[storage] Leitura brokered #" +
                                  std::to_string(sequence) + " iniciada: " +
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
            self->WriteDiagnostic("[storage] Leitura brokered #" +
                                  std::to_string(sequence) + " concluida: " +
                                  std::to_string(loaded) + " bytes");
        }
        return loaded;
    } catch (...) {
        auto self = static_cast<XemuHost*>(opaque);
        if (self) {
            self->WriteDiagnostic("[storage] Excecao durante leitura brokered");
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
