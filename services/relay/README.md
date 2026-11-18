# Golden Balloon relay

This service is the gameplay-packet relay for Vita online multiplayer. It is a
standalone Node.js WebSocket server so the same build can run on a development
laptop and later in Azure App Service. It is separate from the Cloudflare
MatchRoom signaling service: this process carries bounded binary race packets.

The first protocol version provides:

- many simultaneous in-memory rooms, each for two to four endpoints;
- exact game and game-protocol namespaces so one deployment can serve multiple
  integrated games without mixing their rooms or packets;
- a live room directory that a connected Vita can use before hosting or joining;
- an eight-character room code and server-assigned canonical slot;
- host-owned room lifetime (slot 0 leaving closes the room);
- exact, bounded binary packet envelopes with server-injected sender identity;
- destination masks for unicast or fan-out;
- monotonic per-client packet sequences, rate limits, heartbeat cleanup and
  room expiry; and
- `/healthz` for local and Azure health checks.

It does not yet include Vita client wiring, persistent rooms, reconnect leases,
matchmaking or multi-instance state. Production Azure App Service deployment
must initially use one instance. Scaling beyond one instance requires a
backplane such as Azure Web PubSub/SignalR or Redis so all sockets for a room
share one owner.

## Run locally

```powershell
Set-Location services\relay
npm install
npm run check
npm run dev
```

The server listens on `0.0.0.0:8080` by default. Vita clients on the same LAN
will use `ws://<laptop-ip>:8080/relay` with the `mdkr-relay-v1` subprotocol.

Environment settings:

| Name | Default | Meaning |
|---|---:|---|
| `PORT` | `8080` | HTTP/WebSocket port; Azure supplies this setting |
| `HOST` | `0.0.0.0` | Bind address |
| `MDKR_RELAY_MAX_ROOMS` | `1024` | In-memory room cap |
| `MDKR_RELAY_ROOM_TTL_MS` | `21600000` | Idle room expiry, 1 minute to 24 hours |

## Wire outline

The first client text frame is exactly one of:

```json
{"protocolVersion":1,"type":"browse","gameId":"golden-balloon","gameVersion":1}
```

```json
{"protocolVersion":1,"type":"create","gameId":"golden-balloon","gameVersion":1,"playerName":"PLAYER 1","roomName":"ZACH RACE","maxPlayers":4}
```

```json
{"protocolVersion":1,"type":"join","gameId":"golden-balloon","gameVersion":1,"playerName":"PLAYER 2","roomCode":"ABCDEFGH"}
```

`browse` returns a `room_list`, followed by live `room_upsert` and
`room_remove` messages. The same connection can then send `create` or `join`.
Only rooms with the browser's exact `gameId` and `gameVersion` are visible or
joinable. A new game needs no server code change: it chooses a stable lowercase
game ID, increments its compatibility version whenever its relayed wire format
becomes incompatible, and implements this WebSocket client protocol.
After the server's `welcome`, client traffic is binary only. A frame has a
16-byte big-endian header followed by at most 1024 payload bytes:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII `DKRR` |
| 4 | 1 | protocol version `1` |
| 5 | 1 | kind `1` (data) |
| 6 | 1 | sender slot; client sends `255`, server injects `0..3` |
| 7 | 1 | destination slot mask |
| 8 | 1 | channel: session `1`, input `2`, control `3`, repair `4` |
| 9 | 3 | zero |
| 12 | 4 | nonzero monotonically increasing sequence |

The server never trusts a client-provided sender slot. Destination bits without
a currently connected member are ignored, which makes disconnect races safe.
