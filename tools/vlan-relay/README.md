# xemu VLAN coordinator and relay

This service provides endpoint discovery and fallback frame relay for the
UWP-Port VLAN/VPN feature. It has no external Python dependencies.

Run it on a publicly reachable server:

```console
python server.py --host 0.0.0.0 --port 9939
```

Allow inbound and outbound UDP on the selected port. The service keeps room
membership only in memory. A room identifier is a 128-bit bearer secret derived
by clients from the room code; use an unguessable room code. Payloads are not
encrypted by the relay protocol. Production deployments should restrict traffic,
apply rate limits at the firewall, and run one process per public UDP endpoint.

Protocol packets use a 32-byte network-order header:

```text
magic[4] version:u8 type:u8 payload_length:u16 room[16] node:u64
```

Packet types are REGISTER=1, PEERS=2, PING=3, PONG=4, DATA=5, RELAY=6 and
ERROR=255. PEERS contains zero or more `node:u64, IPv4[4], port:u16` records.
Clients send Ethernet frames directly as DATA after hole punching, or send them
to the coordinator as RELAY. The coordinator forwards relayed frames as DATA.
