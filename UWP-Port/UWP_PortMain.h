#pragma once

#include "Common\DeviceResources.h"

// Mantem os recursos da superficie que sera usada pelo renderizador do xemu.
namespace UWP_Port
{
	class UWP_PortMain : public DX::IDeviceNotify
	{
	public:
		UWP_PortMain(const std::shared_ptr<DX::DeviceResources>& deviceResources);
		~UWP_PortMain();
		void CreateWindowSizeDependentResources();
		void StartTracking() { m_tracking = true; }
		void TrackingUpdate(float positionX) { (void)positionX; }
		void StopTracking() { m_tracking = false; }
		bool IsTracking() const { return m_tracking; }
		void StartRenderLoop();
		void StopRenderLoop();
		Concurrency::critical_section& GetCriticalSection() { return m_criticalSection; }

		virtual void OnDeviceLost();
		virtual void OnDeviceRestored();

	private:
		std::shared_ptr<DX::DeviceResources> m_deviceResources;
		Concurrency::critical_section m_criticalSection;
		bool m_tracking = false;
	};
}
