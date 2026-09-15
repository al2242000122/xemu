//
// DirectXPage.xaml.cpp
// Implementação da classe DirectXPage.
//

#include "pch.h"
#include "DirectXPage.xaml.h"

#include <sstream>

using namespace UWP_Port;

using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Storage;
using namespace Windows::Storage::AccessCache;
using namespace Windows::Graphics::Display;
using namespace Windows::System::Threading;
using namespace Windows::UI::Core;
using namespace Windows::UI::Input;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Controls::Primitives;
using namespace Windows::UI::Xaml::Data;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Navigation;
using namespace concurrency;

namespace
{
IPropertySet^ SettingsValues()
{
	return ApplicationData::Current->LocalSettings->Values;
}

bool ReadBool(String^ key, bool fallback)
{
	auto values = SettingsValues();
	return values->HasKey(key) ? safe_cast<bool>(values->Lookup(key)) : fallback;
}

int ReadInt(String^ key, int fallback)
{
	auto values = SettingsValues();
	return values->HasKey(key) ? safe_cast<int>(values->Lookup(key)) : fallback;
}

double ReadDouble(String^ key, double fallback)
{
	auto values = SettingsValues();
	return values->HasKey(key) ? safe_cast<double>(values->Lookup(key)) : fallback;
}

String^ ReadString(String^ key, String^ fallback)
{
	auto values = SettingsValues();
	return values->HasKey(key) ? safe_cast<String^>(values->Lookup(key)) : fallback;
}

std::string Utf8(String^ value)
{
	if (!value || value->IsEmpty()) {
		return {};
	}
	int size = WideCharToMultiByte(CP_UTF8, 0, value->Data(), value->Length(),
	                               nullptr, 0, nullptr, nullptr);
	std::string result(size, '\0');
	WideCharToMultiByte(CP_UTF8, 0, value->Data(), value->Length(),
	                    &result[0], size, nullptr, nullptr);
	return result;
}

std::string TomlString(String^ value)
{
	std::string input = Utf8(value);
	std::string output = "\"";
	for (char c : input) {
		if (c == '\\' || c == '"') output += '\\';
		output += c;
	}
	return output + "\"";
}

const char *BoolText(bool value)
{
	return value ? "true" : "false";
}
}

DirectXPage::DirectXPage():
	m_windowVisible(true),
	m_renderAttached(false),
	m_flashReady(false),
	m_bootromReady(false),
	m_hddReady(false),
	m_dvdReady(false),
	m_savedSystemPointerCursor(nullptr),
	m_systemPointerHidden(false)
{
	InitializeComponent();

	// Registre manipuladores de eventos para o ciclo de vida da página.
	CoreWindow^ window = Window::Current->CoreWindow;

	window->VisibilityChanged +=
		ref new TypedEventHandler<CoreWindow^, VisibilityChangedEventArgs^>(this, &DirectXPage::OnVisibilityChanged);
	m_backRequestedToken = SystemNavigationManager::GetForCurrentView()->BackRequested +=
		ref new EventHandler<BackRequestedEventArgs^>(
			this, &DirectXPage::OnBackRequested);
	m_keyDownToken = window->KeyDown +=
		ref new TypedEventHandler<CoreWindow^, KeyEventArgs^>(
			this, &DirectXPage::OnCoreKeyDown);
	m_keyUpToken = window->KeyUp +=
		ref new TypedEventHandler<CoreWindow^, KeyEventArgs^>(
			this, &DirectXPage::OnCoreKeyUp);

	m_xemu = std::unique_ptr<XemuHost>(new XemuHost());
	LoadSettings();
	RestorePersistedFiles();
	swapChainPanel->Loaded += ref new RoutedEventHandler(
		this, &DirectXPage::OnRenderPanelLoaded);
	swapChainPanel->SizeChanged += ref new SizeChangedEventHandler(
		this, &DirectXPage::OnRenderPanelSizeChanged);
	swapChainPanel->CompositionScaleChanged +=
		ref new TypedEventHandler<SwapChainPanel^, Object^>(
			this, &DirectXPage::OnRenderPanelScaleChanged);
	m_renderingToken = Windows::UI::Xaml::Media::CompositionTarget::Rendering +=
		ref new EventHandler<Object^>(this, &DirectXPage::OnRendering);
}

DirectXPage::~DirectXPage()
{
	// Interrompa a renderização e o processamento de eventos em destruição.
	Windows::UI::Xaml::Media::CompositionTarget::Rendering -= m_renderingToken;
	SystemNavigationManager::GetForCurrentView()->BackRequested -=
		m_backRequestedToken;
	auto window = Window::Current->CoreWindow;
	window->KeyDown -= m_keyDownToken;
	window->KeyUp -= m_keyUpToken;
	m_xemu->Stop();
	if (m_systemPointerHidden) {
		Window::Current->CoreWindow->PointerCursor = m_savedSystemPointerCursor;
	}
}

void DirectXPage::OnRendering(Object^, Object^)
{
	if (m_windowVisible && m_xemu) {
		if (m_xemu->IsRunning()) {
			HideSystemPointer();
		}
		m_xemu->RenderFrame();
	}
}

// Salva o estado atual do aplicativo para eventos de suspensão e de encerramento.
void DirectXPage::SaveInternalState(IPropertySet^ state)
{
	m_xemu->Pause();

	// Coloque aqui o código para salvar o estado do aplicativo.
}

// Carrega o estado atual do aplicativo para eventos de retomada.
void DirectXPage::LoadInternalState(IPropertySet^ state)
{
	// Coloque aqui o código para carregar o estado do aplicativo.

	m_xemu->Resume();
}

// Manipuladores de eventos da janela.

void DirectXPage::OnVisibilityChanged(CoreWindow^ sender, VisibilityChangedEventArgs^ args)
{
	m_windowVisible = args->Visible;
}

void DirectXPage::OnBackRequested(Object^, BackRequestedEventArgs^ args)
{
	args->Handled = true;
}

void DirectXPage::OnCoreKeyDown(CoreWindow^, KeyEventArgs^ args)
{
	if (args->VirtualKey == Windows::System::VirtualKey::GamepadB) {
		args->Handled = true;
	}
}

void DirectXPage::OnCoreKeyUp(CoreWindow^, KeyEventArgs^ args)
{
	if (args->VirtualKey == Windows::System::VirtualKey::GamepadB) {
		args->Handled = true;
	}
}

void DirectXPage::OnRenderPanelLoaded(Object^, RoutedEventArgs^)
{
	if (!m_renderAttached && m_xemu->AttachRenderPanel(swapChainPanel)) {
		m_renderAttached = true;
		m_xemu->UpdateRenderPanelSize(swapChainPanel);
	} else if (!m_renderAttached) {
		auto error = m_xemu->LastError();
		errorText->Text = ref new String(
			std::wstring(error.begin(), error.end()).c_str());
	}
}

void DirectXPage::OnRenderPanelSizeChanged(Object^, SizeChangedEventArgs^)
{
	if (!m_renderAttached && swapChainPanel->IsLoaded) {
		OnRenderPanelLoaded(nullptr, nullptr);
	} else if (m_renderAttached) {
		m_xemu->UpdateRenderPanelSize(swapChainPanel);
	}
}

void DirectXPage::OnRenderPanelScaleChanged(SwapChainPanel^, Object^)
{
	if (m_renderAttached) {
		m_xemu->UpdateRenderPanelSize(swapChainPanel);
	}
}

// Chamado quando você clica no botão da barra de aplicativos.
void DirectXPage::AppBarButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	// Use a barra de aplicativos se ela for apropriada para seu aplicativo. Crie a barra de aplicativos, 
	// depois preencha os manipuladores de eventos (como este).
}

void DirectXPage::NavigationButton_Click(Object^ sender, RoutedEventArgs^)
{
	toolTabs->SelectedIndex = _wtoi(safe_cast<Button^>(sender)->Tag->ToString()->Data());
}

void DirectXPage::FocusEmulatorInput()
{
	startButton->IsTabStop = false;
	launcherPanel->IsHitTestVisible = false;
	this->Focus(Windows::UI::Xaml::FocusState::Programmatic);
}

void DirectXPage::HideSystemPointer()
{
	try {
		auto coreWindow = Window::Current->CoreWindow;
		if (!m_systemPointerHidden) {
			m_savedSystemPointerCursor = coreWindow->PointerCursor;
			m_systemPointerHidden = true;
		}
		if (coreWindow->PointerCursor != nullptr) {
			coreWindow->PointerCursor = nullptr;
		}
	} catch (Platform::Exception^) {
	}
}

void DirectXPage::UpdateStartButtonState()
{
	bool ready = m_flashReady && m_bootromReady && m_hddReady && m_dvdReady;
	startButton->IsEnabled = ready;
	requiredFilesStatus->Text = ready ?
		"Required files are ready. xemu can be started." :
		"Select BIOS, MCPX, hard disk, and DVD/XISO to start.";
	requiredFilesStatus->Foreground = ref new SolidColorBrush(
		ready ? Windows::UI::ColorHelper::FromArgb(255, 76, 195, 138) :
		        Windows::UI::ColorHelper::FromArgb(255, 255, 200, 87));
}

void DirectXPage::StartXemu_Click(Object^, RoutedEventArgs^)
{
	if (!startButton->IsEnabled) {
		return;
	}
	if (!SaveSettings()) {
		toolTabs->SelectedIndex = 4;
		return;
	}
	if (m_xemu->Start()) { hostStatus->Text = "RUNNING"; FocusEmulatorInput(); launcherPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed; HideSystemPointer(); }
	else { auto e = m_xemu->LastError(); errorText->Text = ref new String(std::wstring(e.begin(), e.end()).c_str()); }
}

void DirectXPage::PauseXemu_Click(Object^, RoutedEventArgs^) { m_xemu->Pause(); hostStatus->Text = "PAUSED"; }
void DirectXPage::ResumeXemu_Click(Object^, RoutedEventArgs^) { m_xemu->Resume(); hostStatus->Text = "RUNNING"; }
void DirectXPage::ResetXemu_Click(Object^, RoutedEventArgs^) { m_xemu->Reset(); }
void DirectXPage::StopXemu_Click(Object^, RoutedEventArgs^) { m_xemu->Shutdown(); }

void DirectXPage::LoadSettings()
{
	checkUpdates->IsChecked = ReadBool("general.updates.check", true);
	skipBootAnimation->IsChecked = ReadBool("general.skip_boot_anim", false);
	hardFpu->IsChecked = ReadBool("perf.hard_fpu", true);
	cacheShaders->IsChecked = ReadBool("perf.cache_shaders", true);
	filterSnapshots->IsChecked = ReadBool("general.snapshots.filter_current_game", false);
	autoBind->IsChecked = ReadBool("input.auto_bind", true);
	backgroundInput->IsChecked = ReadBool("input.background_input_capture", false);
	invertLeftX->IsChecked = ReadBool("input.uwp_gamepad.invert_axis_left_x", false);
	invertLeftY->IsChecked = ReadBool("input.uwp_gamepad.invert_axis_left_y", false);
	invertRightX->IsChecked = ReadBool("input.uwp_gamepad.invert_axis_right_x", false);
	invertRightY->IsChecked = ReadBool("input.uwp_gamepad.invert_axis_right_y", false);
	port1Driver->SelectedIndex = ReadInt("input.port1.driver", 0);
	port2Driver->SelectedIndex = ReadInt("input.port2.driver", 0);
	port3Driver->SelectedIndex = ReadInt("input.port3.driver", 0);
	port4Driver->SelectedIndex = ReadInt("input.port4.driver", 0);
	port1SlotA->SelectedIndex = ReadInt("input.port1.slot_a", 0);
	port1SlotB->SelectedIndex = ReadInt("input.port1.slot_b", 0);
	port2SlotA->SelectedIndex = ReadInt("input.port2.slot_a", 0);
	port2SlotB->SelectedIndex = ReadInt("input.port2.slot_b", 0);
	port3SlotA->SelectedIndex = ReadInt("input.port3.slot_a", 0);
	port3SlotB->SelectedIndex = ReadInt("input.port3.slot_b", 0);
	port4SlotA->SelectedIndex = ReadInt("input.port4.slot_a", 0);
	port4SlotB->SelectedIndex = ReadInt("input.port4.slot_b", 0);
	surfaceScale->SelectedIndex = ReadInt("display.quality.surface_scale", 1) - 1;
	filtering->SelectedIndex = ReadInt("display.filtering", 0);
	displayFit->SelectedIndex = ReadInt("display.ui.fit", 1);
	aspectRatio->SelectedIndex = ReadInt("display.ui.aspect_ratio", 1);
	startupSize->SelectedIndex = ReadInt("display.window.startup_size", 5);
	fullscreenStartup->IsChecked = ReadBool("display.window.fullscreen_on_startup", false);
	fullscreenExclusive->IsChecked = ReadBool("display.window.fullscreen_exclusive", false);
	vsync->IsChecked = ReadBool("display.window.vsync", true);
	showMenubar->IsChecked = ReadBool("display.ui.show_menubar", true);
	showNotifications->IsChecked = ReadBool("display.ui.show_notifications", true);
	hideCursor->IsChecked = ReadBool("display.ui.hide_cursor", true);
	useAnimations->IsChecked = ReadBool("display.ui.use_animations", true);
	autoUiScale->IsChecked = ReadBool("display.ui.auto_scale", true);
	uiScale->Value = ReadDouble("display.ui.scale", 1.0);
	useDsp->IsChecked = ReadBool("audio.use_dsp", false);
	useDspJit->IsChecked = ReadBool("audio.use_dsp_jit", false);
	useHrtf->IsChecked = ReadBool("audio.hrtf", true);
	volumeLimit->Value = ReadDouble("audio.volume_limit", 1.0);
	voiceWorkers->Value = ReadInt("audio.vp.num_workers", 0);
	networkEnabled->IsChecked = ReadBool("net.enable", false);
	networkBackend->SelectedIndex = ReadInt("net.backend", 0);
	udpBindAddress->Text = ReadString("net.udp.bind_addr", "0.0.0.0:9368");
	udpRemoteAddress->Text = ReadString("net.udp.remote_addr", "1.2.3.4:9368");
	natForwardPorts->Text = ReadString("net.nat.forward_ports", "");
	memoryLimit->SelectedIndex = ReadInt("sys.mem_limit", 0);
	avPack->SelectedIndex = ReadInt("sys.avpack", 1);
}

void DirectXPage::SaveSettings_Click(Object^, RoutedEventArgs^)
{
	if (SaveSettings()) {
		hostStatus->Text = "SETTINGS SAVED";
	}
}

bool DirectXPage::SaveSettings()
{
	auto values = SettingsValues();
#define SAVE_BOOL(key, control) values->Insert(key, control->IsChecked->Value)
#define SAVE_INT(key, value) values->Insert(key, static_cast<int>(value))
#define SAVE_DOUBLE(key, value) values->Insert(key, static_cast<double>(value))
	SAVE_BOOL("general.updates.check", checkUpdates);
	SAVE_BOOL("general.skip_boot_anim", skipBootAnimation);
	SAVE_BOOL("perf.hard_fpu", hardFpu);
	SAVE_BOOL("perf.cache_shaders", cacheShaders);
	SAVE_BOOL("general.snapshots.filter_current_game", filterSnapshots);
	SAVE_BOOL("input.auto_bind", autoBind);
	SAVE_BOOL("input.background_input_capture", backgroundInput);
	SAVE_BOOL("input.uwp_gamepad.invert_axis_left_x", invertLeftX);
	SAVE_BOOL("input.uwp_gamepad.invert_axis_left_y", invertLeftY);
	SAVE_BOOL("input.uwp_gamepad.invert_axis_right_x", invertRightX);
	SAVE_BOOL("input.uwp_gamepad.invert_axis_right_y", invertRightY);
	SAVE_INT("input.port1.driver", port1Driver->SelectedIndex);
	SAVE_INT("input.port2.driver", port2Driver->SelectedIndex);
	SAVE_INT("input.port3.driver", port3Driver->SelectedIndex);
	SAVE_INT("input.port4.driver", port4Driver->SelectedIndex);
	SAVE_INT("input.port1.slot_a", port1SlotA->SelectedIndex);
	SAVE_INT("input.port1.slot_b", port1SlotB->SelectedIndex);
	SAVE_INT("input.port2.slot_a", port2SlotA->SelectedIndex);
	SAVE_INT("input.port2.slot_b", port2SlotB->SelectedIndex);
	SAVE_INT("input.port3.slot_a", port3SlotA->SelectedIndex);
	SAVE_INT("input.port3.slot_b", port3SlotB->SelectedIndex);
	SAVE_INT("input.port4.slot_a", port4SlotA->SelectedIndex);
	SAVE_INT("input.port4.slot_b", port4SlotB->SelectedIndex);
	SAVE_INT("display.quality.surface_scale", surfaceScale->SelectedIndex + 1);
	SAVE_INT("display.filtering", filtering->SelectedIndex);
	SAVE_INT("display.ui.fit", displayFit->SelectedIndex);
	SAVE_INT("display.ui.aspect_ratio", aspectRatio->SelectedIndex);
	SAVE_INT("display.window.startup_size", startupSize->SelectedIndex);
	SAVE_BOOL("display.window.fullscreen_on_startup", fullscreenStartup);
	SAVE_BOOL("display.window.fullscreen_exclusive", fullscreenExclusive);
	SAVE_BOOL("display.window.vsync", vsync);
	SAVE_BOOL("display.ui.show_menubar", showMenubar);
	SAVE_BOOL("display.ui.show_notifications", showNotifications);
	SAVE_BOOL("display.ui.hide_cursor", hideCursor);
	SAVE_BOOL("display.ui.use_animations", useAnimations);
	SAVE_BOOL("display.ui.auto_scale", autoUiScale);
	SAVE_DOUBLE("display.ui.scale", uiScale->Value);
	SAVE_BOOL("audio.use_dsp", useDsp);
	SAVE_BOOL("audio.use_dsp_jit", useDspJit);
	SAVE_BOOL("audio.hrtf", useHrtf);
	SAVE_DOUBLE("audio.volume_limit", volumeLimit->Value);
	SAVE_INT("audio.vp.num_workers", static_cast<int>(voiceWorkers->Value));
	SAVE_BOOL("net.enable", networkEnabled);
	SAVE_INT("net.backend", networkBackend->SelectedIndex);
	values->Insert("net.udp.bind_addr", udpBindAddress->Text);
	values->Insert("net.udp.remote_addr", udpRemoteAddress->Text);
	values->Insert("net.nat.forward_ports", natForwardPorts->Text);
	SAVE_INT("sys.mem_limit", memoryLimit->SelectedIndex);
	SAVE_INT("sys.avpack", avPack->SelectedIndex);
#undef SAVE_BOOL
#undef SAVE_INT
#undef SAVE_DOUBLE

	static const char *filterValues[] = { "linear", "nearest" };
	static const char *fitValues[] = { "center", "scale", "stretch" };
	static const char *aspectValues[] = { "native", "auto", "4x3", "16x9" };
	static const char *sizeValues[] = { "last_used", "640x480", "720x480", "1280x720", "1280x800", "1280x960", "1920x1080", "2560x1440", "2560x1600", "2560x1920", "3840x2160" };
	static const char *backendValues[] = { "nat", "udp" };
	static const char *avValues[] = { "scart", "hdtv", "vga", "rfu", "svideo", "composite", "none" };
	static const char *controllerDrivers[] = { "usb-xbox-gamepad", "usb-xbox-gamepad-s" };
	auto futureFiles = StorageApplicationPermissions::FutureAccessList;
	auto validateXmu = [this, futureFiles](ComboBox^ slot, String^ tag) {
		if (slot->SelectedIndex == 1 && !futureFiles->ContainsItem("xemu-" + tag)) {
			errorText->Text = "Select the Memory Unit file " + tag +
			                  " on the Folders and Memory Units page.";
			return false;
		}
		return true;
	};
	if (!validateXmu(port1SlotA, "xmu-p1a") ||
	    !validateXmu(port1SlotB, "xmu-p1b") ||
	    !validateXmu(port2SlotA, "xmu-p2a") ||
	    !validateXmu(port2SlotB, "xmu-p2b") ||
	    !validateXmu(port3SlotA, "xmu-p3a") ||
	    !validateXmu(port3SlotB, "xmu-p3b") ||
	    !validateXmu(port4SlotA, "xmu-p4a") ||
	    !validateXmu(port4SlotB, "xmu-p4b")) {
		return false;
	}

	std::ostringstream natRules;
	std::istringstream rules(Utf8(natForwardPorts->Text));
	std::string rule;
	while (std::getline(rules, rule)) {
		if (rule.empty()) continue;
		std::istringstream fields(rule);
		std::string protocol, host, guest, extra;
		if (!std::getline(fields, protocol, ',') ||
		    !std::getline(fields, host, ',') ||
		    !std::getline(fields, guest, ',') || std::getline(fields, extra, ',')) {
			errorText->Text = "Invalid NAT rule. Use: tcp|udp,host port,Xbox port.";
			return false;
		}
		int hostPort = atoi(host.c_str());
		int guestPort = atoi(guest.c_str());
		if ((protocol != "tcp" && protocol != "udp") || hostPort < 1 ||
		    hostPort > 65535 || guestPort < 1 || guestPort > 65535) {
			errorText->Text = "Invalid NAT rule. Use ports from 1 to 65535.";
			return false;
		}
		natRules << "[[net.nat.forward_ports]]\nhost = " << hostPort
		         << "\nguest = " << guestPort << "\nprotocol = \""
		         << protocol << "\"\n";
	}

	std::ostringstream config;
	config << "[general]\n"
	       << "skip_boot_anim = " << BoolText(skipBootAnimation->IsChecked->Value) << "\n"
	       << "screenshot_dir = " << (StorageApplicationPermissions::FutureAccessList->ContainsItem("xemu-screenshots") ? "\"/broker/screenshots\"" : "\"\"") << "\n"
	       << "games_dir = " << (StorageApplicationPermissions::FutureAccessList->ContainsItem("xemu-games") ? "\"/broker/games\"" : "\"\"") << "\n"
	       << "[general.updates]\ncheck = " << BoolText(checkUpdates->IsChecked->Value) << "\n"
	       << "[general.snapshots]\nfilter_current_game = " << BoolText(filterSnapshots->IsChecked->Value) << "\n"
	       << "[perf]\nhard_fpu = " << BoolText(hardFpu->IsChecked->Value) << "\ncache_shaders = " << BoolText(cacheShaders->IsChecked->Value) << "\n"
	       << "[input]\nauto_bind = " << BoolText(autoBind->IsChecked->Value) << "\nbackground_input_capture = " << BoolText(backgroundInput->IsChecked->Value) << "\n"
	       << "[input.bindings]\nport1_driver = \"" << controllerDrivers[port1Driver->SelectedIndex]
	       << "\"\nport2_driver = \"" << controllerDrivers[port2Driver->SelectedIndex]
	       << "\"\nport3_driver = \"" << controllerDrivers[port3Driver->SelectedIndex]
	       << "\"\nport4_driver = \"" << controllerDrivers[port4Driver->SelectedIndex] << "\"\n"
	       << "[input.uwp_gamepad]\ninvert_axis_left_x = " << BoolText(invertLeftX->IsChecked->Value)
	       << "\ninvert_axis_left_y = " << BoolText(invertLeftY->IsChecked->Value)
	       << "\ninvert_axis_right_x = " << BoolText(invertRightX->IsChecked->Value)
	       << "\ninvert_axis_right_y = " << BoolText(invertRightY->IsChecked->Value) << "\n"
	       << "[input.peripherals.port1]\nperipheral_type_0 = " << port1SlotA->SelectedIndex << "\nperipheral_param_0 = \"/broker/xmu-p1a\"\nperipheral_type_1 = " << port1SlotB->SelectedIndex << "\nperipheral_param_1 = \"/broker/xmu-p1b\"\n"
	       << "[input.peripherals.port2]\nperipheral_type_0 = " << port2SlotA->SelectedIndex << "\nperipheral_param_0 = \"/broker/xmu-p2a\"\nperipheral_type_1 = " << port2SlotB->SelectedIndex << "\nperipheral_param_1 = \"/broker/xmu-p2b\"\n"
	       << "[input.peripherals.port3]\nperipheral_type_0 = " << port3SlotA->SelectedIndex << "\nperipheral_param_0 = \"/broker/xmu-p3a\"\nperipheral_type_1 = " << port3SlotB->SelectedIndex << "\nperipheral_param_1 = \"/broker/xmu-p3b\"\n"
	       << "[input.peripherals.port4]\nperipheral_type_0 = " << port4SlotA->SelectedIndex << "\nperipheral_param_0 = \"/broker/xmu-p4a\"\nperipheral_type_1 = " << port4SlotB->SelectedIndex << "\nperipheral_param_1 = \"/broker/xmu-p4b\"\n"
	       << "[display]\nrenderer = \"OPENGL\"\nfiltering = \"" << filterValues[filtering->SelectedIndex] << "\"\n"
	       << "[display.quality]\nsurface_scale = " << surfaceScale->SelectedIndex + 1 << "\n"
	       << "[display.window]\nfullscreen_on_startup = " << BoolText(fullscreenStartup->IsChecked->Value)
	       << "\nfullscreen_exclusive = " << BoolText(fullscreenExclusive->IsChecked->Value)
	       << "\nstartup_size = \"" << sizeValues[startupSize->SelectedIndex] << "\"\nvsync = " << BoolText(vsync->IsChecked->Value) << "\n"
	       << "[display.ui]\nshow_menubar = " << BoolText(showMenubar->IsChecked->Value)
	       << "\nshow_notifications = " << BoolText(showNotifications->IsChecked->Value)
	       << "\nhide_cursor = " << BoolText(hideCursor->IsChecked->Value)
	       << "\nuse_animations = " << BoolText(useAnimations->IsChecked->Value)
	       << "\nfit = \"" << fitValues[displayFit->SelectedIndex] << "\"\naspect_ratio = \"" << aspectValues[aspectRatio->SelectedIndex]
	       << "\"\nscale = " << uiScale->Value << "\nauto_scale = " << BoolText(autoUiScale->IsChecked->Value) << "\n"
	       << "[audio]\nuse_dsp = " << BoolText(useDsp->IsChecked->Value)
	       << "\nuse_dsp_jit = " << BoolText(useDspJit->IsChecked->Value)
	       << "\nhrtf = " << BoolText(useHrtf->IsChecked->Value) << "\nvolume_limit = " << volumeLimit->Value << "\n"
	       << "[audio.vp]\nnum_workers = " << static_cast<int>(voiceWorkers->Value) << "\n"
	       << "[net]\nenable = " << BoolText(networkEnabled->IsChecked->Value) << "\nbackend = \"" << backendValues[networkBackend->SelectedIndex] << "\"\n"
	       << "[net.udp]\nbind_addr = " << TomlString(udpBindAddress->Text) << "\nremote_addr = " << TomlString(udpRemoteAddress->Text) << "\n"
	       << natRules.str()
	       << "[sys]\nmem_limit = \"" << (memoryLimit->SelectedIndex == 0 ? "64" : "128")
	       << "\"\navpack = \"" << avValues[avPack->SelectedIndex] << "\"\n";

	auto path = ApplicationData::Current->LocalFolder->Path + "\\xemu.toml";
	CREATEFILE2_EXTENDED_PARAMETERS parameters = {};
	parameters.dwSize = sizeof(parameters);
	parameters.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
	HANDLE file = CreateFile2(path->Data(), GENERIC_WRITE, FILE_SHARE_READ,
	                          CREATE_ALWAYS, &parameters);
	if (file == INVALID_HANDLE_VALUE) {
		errorText->Text = "Failed to open xemu.toml for writing.";
		return false;
	}
	std::string text = config.str();
	DWORD written = 0;
	bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()),
	                    &written, nullptr) && written == text.size();
	CloseHandle(file);
	if (!ok) {
		errorText->Text = "Failed to save all settings to xemu.toml.";
	}
	return ok;
}

void DirectXPage::SelectFile_Click(Object^ sender, RoutedEventArgs^)
{
	auto button = safe_cast<Button^>(sender);
	auto picker = ref new Windows::Storage::Pickers::FileOpenPicker();
	picker->FileTypeFilter->Append("*");
	create_task(picker->PickSingleFileAsync()).then([this, button](Windows::Storage::StorageFile^ file) {
		if (!file) return;
		auto tagValue = button->Tag->ToString();
	MountXboxFile(file, tagValue, tagValue != "dvd");
	});
}

void DirectXPage::SelectFolder_Click(Object^ sender, RoutedEventArgs^)
{
	auto button = safe_cast<Button^>(sender);
	auto picker = ref new Windows::Storage::Pickers::FolderPicker();
	picker->FileTypeFilter->Append("*");
	create_task(picker->PickSingleFolderAsync()).then(
		[this, button](StorageFolder^ folder) {
			if (folder) MountXboxFolder(folder, button->Tag->ToString(), true);
		});
}

void DirectXPage::MountXboxFolder(StorageFolder^ folder, String^ tagValue,
	                               bool persist)
{
	auto status = tagValue == "screenshots" ? screenshotFolderStatus :
	                                           gamesFolderStatus;
	status->Text = folder->Path->IsEmpty() ? folder->Name : folder->Path;
	if (persist) {
		StorageApplicationPermissions::FutureAccessList->AddOrReplace(
			"xemu-" + tagValue, folder);
	}
	std::string tag = tagValue == "screenshots" ? "screenshots" : "games";
	if (!m_xemu->MountFolder("/broker/" + tag, folder)) {
		errorText->Text = "Failed to mount the selected folder.";
		toolTabs->SelectedIndex = 4;
	}
}

void DirectXPage::MountXboxFile(StorageFile^ file, String^ tagValue,
	                            bool persist)
{
	auto status = tagValue == "flash" ? flashFileStatus :
	              tagValue == "bootrom" ? bootromFileStatus :
	              tagValue == "hdd" ? hddFileStatus :
	              tagValue == "eeprom" ? eepromFileStatus :
	              tagValue == "xmu-p1a" ? xmuP1AStatus :
	              tagValue == "xmu-p1b" ? xmuP1BStatus :
	              tagValue == "xmu-p2a" ? xmuP2AStatus :
	              tagValue == "xmu-p2b" ? xmuP2BStatus :
	              tagValue == "xmu-p3a" ? xmuP3AStatus :
	              tagValue == "xmu-p3b" ? xmuP3BStatus :
	              tagValue == "xmu-p4a" ? xmuP4AStatus :
	              tagValue == "xmu-p4b" ? xmuP4BStatus : dvdFileStatus;
	auto location = file->Path->IsEmpty() ? file->Name : file->Path;
	status->Text = location + "  |  " + file->Name;
	if (tagValue == "xmu-p1a") port1SlotA->SelectedIndex = 1;
	else if (tagValue == "xmu-p1b") port1SlotB->SelectedIndex = 1;
	else if (tagValue == "xmu-p2a") port2SlotA->SelectedIndex = 1;
	else if (tagValue == "xmu-p2b") port2SlotB->SelectedIndex = 1;
	else if (tagValue == "xmu-p3a") port3SlotA->SelectedIndex = 1;
	else if (tagValue == "xmu-p3b") port3SlotB->SelectedIndex = 1;
	else if (tagValue == "xmu-p4a") port4SlotA->SelectedIndex = 1;
	else if (tagValue == "xmu-p4b") port4SlotB->SelectedIndex = 1;

	if (persist) {
		StorageApplicationPermissions::FutureAccessList->AddOrReplace(
			"xemu-" + tagValue, file);
	}
	bool isXmu = tagValue->Length() >= 4 &&
	             wcsncmp(tagValue->Data(), L"xmu-", 4) == 0;
	auto access = (tagValue == "hdd" || tagValue == "eeprom" || isXmu) ? FileAccessMode::ReadWrite :
	                                  FileAccessMode::Read;
	create_task(file->OpenAsync(access)).then(
		[this, file, tagValue](Windows::Storage::Streams::IRandomAccessStream^ stream) {
			std::string tag = Utf8(tagValue);
			if (!m_xemu->MountFile("/broker/" + tag, file, stream)) {
				if (tagValue == "flash") m_flashReady = false;
				else if (tagValue == "bootrom") m_bootromReady = false;
				else if (tagValue == "hdd") m_hddReady = false;
				else if (tagValue == "dvd") m_dvdReady = false;
				UpdateStartButtonState();
				auto error = m_xemu->LastError();
				errorText->Text = ref new String(
					std::wstring(error.begin(), error.end()).c_str());
				toolTabs->SelectedIndex = 4;
				return;
			}
			if (tagValue == "flash") m_flashReady = true;
			else if (tagValue == "bootrom") m_bootromReady = true;
			else if (tagValue == "hdd") m_hddReady = true;
			else if (tagValue == "dvd") m_dvdReady = true;
			UpdateStartButtonState();
		});
}

void DirectXPage::RestorePersistedFiles()
{
	RestorePersistedFile("flash");
	RestorePersistedFile("bootrom");
	RestorePersistedFile("hdd");
	RestorePersistedFile("eeprom");
	RestorePersistedFile("xmu-p1a");
	RestorePersistedFile("xmu-p1b");
	RestorePersistedFile("xmu-p2a");
	RestorePersistedFile("xmu-p2b");
	RestorePersistedFile("xmu-p3a");
	RestorePersistedFile("xmu-p3b");
	RestorePersistedFile("xmu-p4a");
	RestorePersistedFile("xmu-p4b");
	RestorePersistedFolder("screenshots");
	RestorePersistedFolder("games");
}

void DirectXPage::RestorePersistedFile(String^ tagValue)
{
	auto token = "xemu-" + tagValue;
	if (!StorageApplicationPermissions::FutureAccessList->ContainsItem(token)) {
		return;
	}
	create_task(StorageApplicationPermissions::FutureAccessList->GetFileAsync(token))
		.then([this, tagValue](StorageFile^ file) {
			MountXboxFile(file, tagValue, false);
		}).then([this](task<void> result) {
			try {
				result.get();
			}
			catch (Platform::Exception^ exception)
			{
				errorText->Text = "Failed to restore saved file: " +
				                  exception->Message;
			}
		});
}

void DirectXPage::RestorePersistedFolder(String^ tagValue)
{
	auto token = "xemu-" + tagValue;
	if (!StorageApplicationPermissions::FutureAccessList->ContainsItem(token)) {
		return;
	}
	create_task(StorageApplicationPermissions::FutureAccessList->GetFolderAsync(token))
		.then([this, tagValue](StorageFolder^ folder) {
			MountXboxFolder(folder, tagValue, false);
		}).then([this](task<void> result) {
			try { result.get(); }
			catch (Platform::Exception^ exception) {
				errorText->Text = "Failed to restore saved folder: " + exception->Message;
			}
		});
}

