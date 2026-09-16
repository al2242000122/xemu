#!/usr/bin/env python3
"""xemu UWP VLAN coordinator and UDP relay."""

import argparse
import asyncio
import logging
import struct
import time

MAGIC = b"XVL1"
VERSION = 1
HEADER = struct.Struct("!4sBBH16sQ")
REGISTER = 1
PEERS = 2
PING = 3
PONG = 4
DATA = 5
RELAY = 6
ERROR = 255
PEER = struct.Struct("!Q4sH")
MAX_DATAGRAM = 65507


class VlanRelay(asyncio.DatagramProtocol):
    def __init__(self, timeout: float, max_rooms: int, max_peers: int):
        self.timeout = timeout
        self.max_rooms = max_rooms
        self.max_peers = max_peers
        self.transport = None
        self.rooms = {}

    def connection_made(self, transport):
        self.transport = transport
        logging.info("VLAN relay listening on %s", transport.get_extra_info("sockname"))

    def _packet(self, kind, room, node, payload=b""):
        return HEADER.pack(MAGIC, VERSION, kind, len(payload), room, node) + payload

    def _prune(self):
        cutoff = time.monotonic() - self.timeout
        for room, peers in list(self.rooms.items()):
            for node, (_, seen) in list(peers.items()):
                if seen < cutoff:
                    del peers[node]
            if not peers:
                del self.rooms[room]

    def _send_peers(self, room):
        peers = self.rooms.get(room, {})
        for recipient, (address, _) in peers.items():
            payload = bytearray()
            for node, (peer_address, _) in peers.items():
                if node == recipient or ":" in peer_address[0]:
                    continue
                try:
                    octets = bytes(int(part) for part in peer_address[0].split("."))
                except (ValueError, OverflowError):
                    continue
                if len(octets) == 4:
                    payload += PEER.pack(node, octets, peer_address[1])
            self.transport.sendto(self._packet(PEERS, room, 0, payload), address)

    def datagram_received(self, datagram, address):
        if len(datagram) < HEADER.size or len(datagram) > MAX_DATAGRAM:
            return
        magic, version, kind, length, room, node = HEADER.unpack_from(datagram)
        payload = datagram[HEADER.size:]
        if magic != MAGIC or version != VERSION or length != len(payload) or node == 0:
            return

        self._prune()
        if kind == REGISTER:
            peers = self.rooms.get(room)
            if peers is None:
                if len(self.rooms) >= self.max_rooms:
                    self.transport.sendto(self._packet(ERROR, room, 0, b"room limit"), address)
                    return
                peers = self.rooms.setdefault(room, {})
            if node not in peers and len(peers) >= self.max_peers:
                self.transport.sendto(self._packet(ERROR, room, 0, b"room full"), address)
                return
            peers[node] = (address, time.monotonic())
            self._send_peers(room)
            return

        peers = self.rooms.get(room)
        if not peers or node not in peers or peers[node][0] != address:
            return
        peers[node] = (address, time.monotonic())
        if kind == PING:
            self.transport.sendto(self._packet(PONG, room, 0, payload), address)
        elif kind == RELAY:
            packet = self._packet(DATA, room, node, payload)
            for peer, (peer_address, _) in peers.items():
                if peer != node:
                    self.transport.sendto(packet, peer_address)


async def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=9939)
    parser.add_argument("--peer-timeout", type=float, default=15.0)
    parser.add_argument("--max-rooms", type=int, default=1024)
    parser.add_argument("--max-peers", type=int, default=16)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    loop = asyncio.get_running_loop()
    transport, _ = await loop.create_datagram_endpoint(
        lambda: VlanRelay(args.peer_timeout, args.max_rooms, args.max_peers),
        local_addr=(args.host, args.port))
    try:
        await asyncio.Future()
    finally:
        transport.close()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
