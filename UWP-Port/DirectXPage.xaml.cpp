//
// DirectXPage.xaml.cpp
// Implementação da classe DirectXPage.
//

#include "pch.h"
#include "DirectXPage.xaml.h"

using namespace UWP_Port;

using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
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

DirectXPage::DirectXPage():
	m_windowVisible(true),
	m_coreInput(nullptr),
	m_renderAttached(false)
{
	InitializeComponent();

	// Registre manipuladores de eventos para o ciclo de vida da página.
	CoreWindow^ window = Window::Current->CoreWindow;

	window->VisibilityChanged +=
		ref new TypedEventHandler<CoreWindow^, VisibilityChangedEventArgs^>(this, &DirectXPage::OnVisibilityChanged);

	// Registre nosso SwapChainPanel para obter eventos de ponteiro de entrada independentes
	auto workItemHandler = ref new WorkItemHandler([this] (IAsyncAction ^)
	{
		// A CoreIndependentInputSource gerará eventos de ponteiro para os tipos de dispositivo especificados em qualquer thread em que é criada.
		m_coreInput = swapChainPanel->CreateCoreIndependentInputSource(
			Windows::UI::Core::CoreInputDeviceTypes::Mouse |
			Windows::UI::Core::CoreInputDeviceTypes::Touch |
			Windows::UI::Core::CoreInputDeviceTypes::Pen
			);

		// Registre-se para eventos de ponteiros, os quais serão gerados no thread em segundo plano.
		m_coreInput->PointerPressed += ref new TypedEventHandler<Object^, PointerEventArgs^>(this, &DirectXPage::OnPointerPressed);
		m_coreInput->PointerMoved += ref new TypedEventHandler<Object^, PointerEventArgs^>(this, &DirectXPage::OnPointerMoved);
		m_coreInput->PointerReleased += ref new TypedEventHandler<Object^, PointerEventArgs^>(this, &DirectXPage::OnPointerReleased);

		// Comece a processar as mensagens de entrada conforme forem entregues.
		m_coreInput->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessUntilQuit);
	});

	// Execute a tarefa em um thread de segundo plano dedicado de alta prioridade.
	m_inputLoopWorker = ThreadPool::RunAsync(workItemHandler, WorkItemPriority::High, WorkItemOptions::TimeSliced);

	m_xemu = std::unique_ptr<XemuHost>(new XemuHost());
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
	m_xemu->Stop();
	if (m_coreInput) {
		m_coreInput->Dispatcher->StopProcessEvents();
	}
}

void DirectXPage::OnRendering(Object^, Object^)
{
	if (m_windowVisible && m_xemu) {
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

void DirectXPage::StartXemu_Click(Object^, RoutedEventArgs^)
{
	if (m_xemu->Start()) { hostStatus->Text = "EXECUTANDO"; launcherPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed; }
	else { auto e = m_xemu->LastError(); errorText->Text = ref new String(std::wstring(e.begin(), e.end()).c_str()); }
}

void DirectXPage::PauseXemu_Click(Object^, RoutedEventArgs^) { m_xemu->Pause(); hostStatus->Text = "PAUSADO"; }
void DirectXPage::ResumeXemu_Click(Object^, RoutedEventArgs^) { m_xemu->Resume(); hostStatus->Text = "EXECUTANDO"; }
void DirectXPage::ResetXemu_Click(Object^, RoutedEventArgs^) { m_xemu->Reset(); }
void DirectXPage::StopXemu_Click(Object^, RoutedEventArgs^) { m_xemu->Shutdown(); }

void DirectXPage::SelectFile_Click(Object^ sender, RoutedEventArgs^)
{
	auto button = safe_cast<Button^>(sender);
	auto picker = ref new Windows::Storage::Pickers::FileOpenPicker();
	picker->FileTypeFilter->Append("*");
	create_task(picker->PickSingleFileAsync()).then([this, button](Windows::Storage::StorageFile^ file) {
		if (!file) return;
		auto tagValue = button->Tag->ToString();
		auto status = tagValue == "flash" ? flashFileStatus :
		              tagValue == "bootrom" ? bootromFileStatus :
		              tagValue == "hdd" ? hddFileStatus : dvdFileStatus;
		auto location = file->Path->IsEmpty() ? file->Name : file->Path;
		status->Text = location + "  |  " + file->Name;
		auto access = tagValue == "hdd" ? Windows::Storage::FileAccessMode::ReadWrite :
		                                  Windows::Storage::FileAccessMode::Read;
		create_task(file->OpenAsync(access)).then([this, file, tagValue](Windows::Storage::Streams::IRandomAccessStream^ stream) {
			std::string tag = tagValue == "flash" ? "flash" :
			                  tagValue == "bootrom" ? "bootrom" :
			                  tagValue == "hdd" ? "hdd" : "dvd";
			if (!m_xemu->MountFile("/broker/" + tag, file, stream)) {
				auto error = m_xemu->LastError();
				errorText->Text = ref new String(
					std::wstring(error.begin(), error.end()).c_str());
				toolTabs->SelectedIndex = 3;
			}
		});
	});
}

void DirectXPage::OnPointerPressed(Object^ sender, PointerEventArgs^ e)
{
	(void)sender;
	(void)e;
}

void DirectXPage::OnPointerMoved(Object^ sender, PointerEventArgs^ e)
{
	(void)sender;
	(void)e;
}

void DirectXPage::OnPointerReleased(Object^ sender, PointerEventArgs^ e)
{
	(void)sender;
	(void)e;
}
