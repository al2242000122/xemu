#include "pch.h"
#include "UWP_PortMain.h"

using namespace UWP_Port;

UWP_PortMain::UWP_PortMain(const std::shared_ptr<DX::DeviceResources>& deviceResources)
	: m_deviceResources(deviceResources)
{
	m_deviceResources->RegisterDeviceNotify(this);
}

UWP_PortMain::~UWP_PortMain()
{
	m_deviceResources->RegisterDeviceNotify(nullptr);
}

void UWP_PortMain::CreateWindowSizeDependentResources()
{
}

void UWP_PortMain::StartRenderLoop()
{
	// O SwapChainPanel pertence ao renderizador OpenGL/Mesa do xemu.
}

void UWP_PortMain::StopRenderLoop()
{
}

void UWP_PortMain::OnDeviceLost()
{
}

void UWP_PortMain::OnDeviceRestored()
{
	CreateWindowSizeDependentResources();
}
