#include "pch.h"
#include "VLanManager.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <vector>

using namespace Windows::Security::Cryptography;

namespace
{
constexpr uint8_t kRegister = 1;
constexpr uint8_t kPeers = 2;
constexpr uint8_t kPing = 3;
constexpr uint8_t kPong = 4;
constexpr uint8_t kData = 5;
constexpr uint8_t kRelay = 6;
constexpr size_t kHeaderSize = 32;
constexpr size_t kMaxFrame = 16384;

uint64_t TickMs()
{
    return GetTickCount64();
}

void Put16(uint8_t* p, uint16_t value)
{
    p[0] = static_cast<uint8_t>(value >> 8);
    p[1] = static_cast<uint8_t>(value);
}

uint16_t Get16(const uint8_t* p)
{
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

void Put64(uint8_t* p, uint64_t value)
{
    for (int i = 7; i >= 0; --i) {
        p[7 - i] = static_cast<uint8_t>(value >> (i * 8));
    }
}

uint64_t Get64(const uint8_t* p)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value = (value << 8) | p[i];
    return value;
}

bool SameAddress(const sockaddr_in& a, const sockaddr_in& b)
{
    return a.sin_port == b.sin_port && a.sin_addr.s_addr == b.sin_addr.s_addr;
}
}

namespace UWP_Port
{
VLanManager::VLanManager() :
    m_running(false), m_stop(false), m_node(0), m_wsaReady(false),
    m_localSocket(INVALID_SOCKET),
    m_networkSocket(INVALID_SOCKET), m_peerCount(0), m_havePeer(false), m_direct(false),
    m_lastDirectTick(0)
{
    WSADATA data = {};
    m_wsaReady = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    memset(&m_coordinator, 0, sizeof(m_coordinator));
    memset(&m_qemu, 0, sizeof(m_qemu));
    memset(&m_peer, 0, sizeof(m_peer));
}

VLanManager::~VLanManager()
{
    Stop();
    if (m_wsaReady) WSACleanup();
}

std::string VLanManager::CreateRoomCode()
{
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i) {
        out << std::setw(8) << CryptographicBuffer::GenerateRandomNumber();
    }
    return out.str();
}

bool VLanManager::IsValidRoomCode(const std::string& code)
{
    return code.size() == 32 && std::all_of(code.begin(), code.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
               (c >= 'A' && c <= 'F');
    });
}

bool VLanManager::Start(const std::string& coordinator, const std::string& roomCode)
{
    Stop();
    if (!m_wsaReady || coordinator.empty() || !IsValidRoomCode(roomCode)) {
        SetStatus("Invalid coordinator or room code");
        return false;
    }
    for (size_t i = 0; i < m_room.size(); ++i) {
        m_room[i] = static_cast<uint8_t>(std::stoul(roomCode.substr(i * 2, 2), nullptr, 16));
    }
    m_node = (static_cast<uint64_t>(CryptographicBuffer::GenerateRandomNumber()) << 32) |
             CryptographicBuffer::GenerateRandomNumber();
    if (!m_node) m_node = 1;
    m_coordinatorName = coordinator;
    m_stop = false;
    m_thread = std::thread(&VLanManager::Run, this);
    return true;
}

void VLanManager::Stop()
{
    m_stop = true;
    if (m_thread.joinable()) m_thread.join();
    if (m_localSocket != INVALID_SOCKET) closesocket(m_localSocket);
    if (m_networkSocket != INVALID_SOCKET) closesocket(m_networkSocket);
    m_localSocket = m_networkSocket = INVALID_SOCKET;
    m_running = false;
}

std::string VLanManager::Status() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

void VLanManager::SetStatus(const std::string& status)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_status = status;
}

bool VLanManager::ResolveCoordinator()
{
    auto split = m_coordinatorName.rfind(':');
    if (split == std::string::npos) return false;
    std::string host = m_coordinatorName.substr(0, split);
    std::string port = m_coordinatorName.substr(split + 1);
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0 || !result) return false;
    memcpy(&m_coordinator, result->ai_addr, sizeof(m_coordinator));
    freeaddrinfo(result);
    return true;
}

bool VLanManager::SendPacket(SOCKET socket, const sockaddr_in& address,
                             uint8_t type, const void* data, size_t size)
{
    if (size > 65503 - kHeaderSize) return false;
    std::vector<uint8_t> packet(kHeaderSize + size);
    memcpy(packet.data(), "XVL1", 4);
    packet[4] = 1;
    packet[5] = type;
    Put16(packet.data() + 6, static_cast<uint16_t>(size));
    memcpy(packet.data() + 8, m_room.data(), m_room.size());
    Put64(packet.data() + 24, m_node);
    if (size) memcpy(packet.data() + kHeaderSize, data, size);
    return sendto(socket, reinterpret_cast<const char*>(packet.data()),
                  static_cast<int>(packet.size()), 0,
                  reinterpret_cast<const sockaddr*>(&address), sizeof(address)) >= 0;
}

void VLanManager::SendRegistration()
{
    SendPacket(m_networkSocket, m_coordinator, kRegister, nullptr, 0);
}

void VLanManager::HandleNetworkPacket(const uint8_t* packet, size_t size,
                                      const sockaddr_in& source)
{
    if (size < kHeaderSize || memcmp(packet, "XVL1", 4) || packet[4] != 1 ||
        Get16(packet + 6) != size - kHeaderSize ||
        memcmp(packet + 8, m_room.data(), m_room.size())) return;
    uint8_t type = packet[5];
    const uint8_t* payload = packet + kHeaderSize;
    size_t payloadSize = size - kHeaderSize;
    if (type == kPeers && SameAddress(source, m_coordinator)) {
        m_peerCount = payloadSize / 14;
        if (m_peerCount == 1 && Get64(payload) != m_node) {
            memset(&m_peer, 0, sizeof(m_peer));
            m_peer.sin_family = AF_INET;
            memcpy(&m_peer.sin_addr, payload + 8, 4);
            memcpy(&m_peer.sin_port, payload + 12, 2);
            m_havePeer = true;
            SendPacket(m_networkSocket, m_peer, kPing, nullptr, 0);
            SetStatus("Peer found; testing direct UDP");
        } else if (m_peerCount > 1) {
            m_havePeer = m_direct = false;
            SetStatus("Connected using relay (multi-peer room)");
        } else {
            m_havePeer = m_direct = false;
            SetStatus("Waiting for another room member");
        }
    } else if (type == kPing && !SameAddress(source, m_coordinator)) {
        m_peer = source;
        m_havePeer = true;
        SendPacket(m_networkSocket, source, kPong, nullptr, 0);
    } else if (type == kPong && !SameAddress(source, m_coordinator)) {
        m_peer = source;
        m_havePeer = m_direct = true;
        m_lastDirectTick = TickMs();
        SetStatus("Connected using direct UDP");
    } else if (type == kData && payloadSize && payloadSize <= kMaxFrame) {
        if (!SameAddress(source, m_coordinator)) {
            m_peer = source;
            m_havePeer = m_direct = true;
            m_lastDirectTick = TickMs();
            SetStatus("Connected using direct UDP");
        }
        sendto(m_localSocket, reinterpret_cast<const char*>(payload),
               static_cast<int>(payloadSize), 0,
               reinterpret_cast<const sockaddr*>(&m_qemu), sizeof(m_qemu));
    }
}

void VLanManager::Run()
{
    m_running = true;
    SetStatus("Starting VLAN/VPN");
    if (!ResolveCoordinator()) {
        SetStatus("Could not resolve coordinator");
        m_running = false;
        return;
    }
    m_localSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    m_networkSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = htons(9940);
    sockaddr_in network = {};
    network.sin_family = AF_INET;
    network.sin_addr.s_addr = htonl(INADDR_ANY);
    network.sin_port = 0;
    m_qemu.sin_family = AF_INET;
    m_qemu.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    m_qemu.sin_port = htons(9941);
    if (m_localSocket == INVALID_SOCKET || m_networkSocket == INVALID_SOCKET ||
        bind(m_localSocket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) ||
        bind(m_networkSocket, reinterpret_cast<sockaddr*>(&network), sizeof(network))) {
        SetStatus("Could not bind VLAN UDP sockets");
        if (m_localSocket != INVALID_SOCKET) closesocket(m_localSocket);
        if (m_networkSocket != INVALID_SOCKET) closesocket(m_networkSocket);
        m_localSocket = m_networkSocket = INVALID_SOCKET;
        m_running = false;
        return;
    }
    uint64_t nextRegister = 0;
    std::array<uint8_t, 65535> buffer;
    while (!m_stop) {
        uint64_t now = TickMs();
        if (now >= nextRegister) {
            SendRegistration();
            if (m_havePeer) SendPacket(m_networkSocket, m_peer, kPing, nullptr, 0);
            nextRegister = now + 2000;
        }
        if (m_direct && now - m_lastDirectTick > 6000) {
            m_direct = false;
            SetStatus("Connected using relay fallback");
        }
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(m_localSocket, &readfds);
        FD_SET(m_networkSocket, &readfds);
        timeval timeout = { 0, 200000 };
        int ready = select(0, &readfds, nullptr, nullptr, &timeout);
        if (ready <= 0) continue;
        if (FD_ISSET(m_localSocket, &readfds)) {
            int received = recv(m_localSocket, reinterpret_cast<char*>(buffer.data()),
                                static_cast<int>(buffer.size()), 0);
            if (received > 0) {
                if (m_direct && m_havePeer) {
                    SendPacket(m_networkSocket, m_peer, kData, buffer.data(), received);
                } else {
                    SendPacket(m_networkSocket, m_coordinator, kRelay, buffer.data(), received);
                    SetStatus("Connected using relay fallback");
                }
            }
        }
        if (FD_ISSET(m_networkSocket, &readfds)) {
            sockaddr_in source = {};
            int sourceSize = sizeof(source);
            int received = recvfrom(m_networkSocket, reinterpret_cast<char*>(buffer.data()),
                                    static_cast<int>(buffer.size()), 0,
                                    reinterpret_cast<sockaddr*>(&source), &sourceSize);
            if (received > 0) HandleNetworkPacket(buffer.data(), received, source);
        }
    }
    closesocket(m_localSocket);
    closesocket(m_networkSocket);
    m_localSocket = m_networkSocket = INVALID_SOCKET;
    m_running = false;
}
}
