import asyncio
import socket
import struct
import unittest

import server


class RelayTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        loop = asyncio.get_running_loop()
        self.transport, _ = await loop.create_datagram_endpoint(
            lambda: server.VlanRelay(15, 8, 4), local_addr=("127.0.0.1", 0))
        self.address = self.transport.get_extra_info("sockname")
        self.room = bytes(range(16))

    async def asyncTearDown(self):
        self.transport.close()

    def packet(self, kind, node, payload=b""):
        return server.HEADER.pack(server.MAGIC, 1, kind, len(payload), self.room, node) + payload

    async def receive(self, sock):
        loop = asyncio.get_running_loop()
        return await asyncio.wait_for(loop.sock_recvfrom(sock, 65535), 1)

    async def test_register_discovery_and_relay(self):
        a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for sock in (a, b):
            sock.setblocking(False)
            sock.bind(("127.0.0.1", 0))
        try:
            a.sendto(self.packet(server.REGISTER, 1), self.address)
            await self.receive(a)
            b.sendto(self.packet(server.REGISTER, 2), self.address)
            await self.receive(a)
            _, _ = await self.receive(b)
            frame = b"ethernet frame"
            a.sendto(self.packet(server.RELAY, 1, frame), self.address)
            relayed, _ = await self.receive(b)
            magic, version, kind, length, room, node = server.HEADER.unpack_from(relayed)
            self.assertEqual((magic, version, kind, room, node),
                             (server.MAGIC, 1, server.DATA, self.room, 1))
            self.assertEqual(relayed[server.HEADER.size:], frame)
            self.assertEqual(length, len(frame))
        finally:
            a.close()
            b.close()

    async def test_relay_broadcasts_to_every_other_room_member(self):
        sockets = [socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                   for _ in range(3)]
        for sock in sockets:
            sock.setblocking(False)
            sock.bind(("127.0.0.1", 0))
        try:
            for node, sock in enumerate(sockets, 1):
                sock.sendto(self.packet(server.REGISTER, node), self.address)
                for registered in sockets[:node]:
                    await self.receive(registered)
            frame = b"broadcast ethernet frame"
            sockets[0].sendto(self.packet(server.RELAY, 1, frame), self.address)
            for recipient in sockets[1:]:
                relayed, _ = await self.receive(recipient)
                self.assertEqual(relayed[server.HEADER.size:], frame)
        finally:
            for sock in sockets:
                sock.close()


if __name__ == "__main__":
    unittest.main()
