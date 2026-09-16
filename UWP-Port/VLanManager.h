#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace UWP_Port
{
    class VLanManager final
    {
    public:
        VLanManager();
        ~VLanManager();
        bool Start(const std::string& coordinator, const std::string& roomCode);
        void Stop();
        bool IsRunning() const { return m_running.load(); }
        std::string Status() const;
        static std::string CreateRoomCode();
        static bool IsValidRoomCode(const std::string& roomCode);

    private:
        void Run();
        void SetStatus(const std::string& status);
        bool ResolveCoordinator();
        bool SendPacket(SOCKET socket, const sockaddr_in& address, uint8_t type,
                        const void* data, size_t size);
        void HandleNetworkPacket(const uint8_t* data, size_t size,
                                 const sockaddr_in& source);
        void SendRegistration();

        std::atomic<bool> m_running;
        std::atomic<bool> m_stop;
        std::thread m_thread;
        mutable std::mutex m_mutex;
        std::string m_status;
        std::string m_coordinatorName;
        std::array<uint8_t, 16> m_room;
        uint64_t m_node;
        bool m_wsaReady;
        SOCKET m_localSocket;
        SOCKET m_networkSocket;
        sockaddr_in m_coordinator;
        sockaddr_in m_qemu;
        sockaddr_in m_peer;
        size_t m_peerCount;
        bool m_havePeer;
        bool m_direct;
        uint64_t m_lastDirectTick;
    };
}
