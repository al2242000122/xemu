//
// DirectXPage.xaml.h
// Declaração da classe DirectXPage.
//

#pragma once

#include "DirectXPage.g.h"

#include "XemuHost.h"

namespace UWP_Port
{
	/// <summary>
	/// Uma página que hospeda um SwapChainPanel do DirectX.
	/// </summary>
	public ref class DirectXPage sealed
	{
	public:
		DirectXPage();
		virtual ~DirectXPage();

		void SaveInternalState(Windows::Foundation::Collections::IPropertySet^ state);
		void LoadInternalState(Windows::Foundation::Collections::IPropertySet^ state);

	private:
		// Manipulador de eventos de renderização de baixo nível XAML.
		void OnRendering(Platform::Object^ sender, Platform::Object^ args);

		// Manipuladores de eventos da janela.
		void OnVisibilityChanged(Windows::UI::Core::CoreWindow^ sender, Windows::UI::Core::VisibilityChangedEventArgs^ args);
		void OnRenderPanelLoaded(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
		void OnRenderPanelSizeChanged(Platform::Object^ sender, Windows::UI::Xaml::SizeChangedEventArgs^ args);
		void OnRenderPanelScaleChanged(Windows::UI::Xaml::Controls::SwapChainPanel^ sender, Platform::Object^ args);

		// Outros manipuladores de eventos.
		void AppBarButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void NavigationButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void StartXemu_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void PauseXemu_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void ResumeXemu_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void ResetXemu_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void StopXemu_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void SelectFile_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		// Rastreie nossa entrada independente em um thread de trabalho de segundo plano.
		Windows::Foundation::IAsyncAction^ m_inputLoopWorker;
		Windows::UI::Core::CoreIndependentInputSource^ m_coreInput;

		// Funções de manipulação de entrada independente.
		void OnPointerPressed(Platform::Object^ sender, Windows::UI::Core::PointerEventArgs^ e);
		void OnPointerMoved(Platform::Object^ sender, Windows::UI::Core::PointerEventArgs^ e);
		void OnPointerReleased(Platform::Object^ sender, Windows::UI::Core::PointerEventArgs^ e);

		std::unique_ptr<XemuHost> m_xemu;
		Windows::Foundation::EventRegistrationToken m_renderingToken;
		bool m_windowVisible;
		bool m_renderAttached;
	};
}

