# p2p~

A Pure Data external for peer-to-peer audio streaming using WebRTC.

## Installation

1. Build the external (requires: libopus, nlohmann-json, libdatachannel, spdlog, boost)
2. Copy `p2p~.pd_linux` (or `.dll` / `.dylib`) to your Pd externals folder

## Basic Usage

```
[p2p~] - create object
[connect wss://your-server.com roomname username( - connect to room
[stream 1( - start/stop streaming (1=on, 0=off)
[disconnect( - leave room
[message hello world( - send chat message
```

## Connection Flow

1. Create `[p2p~]`
2. Send `connect URL room username`
3. Audio automatically streams between peers
4. Output shows peer count via `peers` message

## Outputs

- Left outlet: audio signals
- Right outlet: messages (`peers`, `json` data)

## Options

| Flag | Description |
|------|-------------|
| `-o N` | Multi-channel mode with N outputs |
| `-f` | Fixed channel mapping with `setchannel` |
| `-json key` | Parse incoming JSON messages |

## Examples

Basic stereo:
```
[p2p~]
[connect wss://myserver.com lobby alice(
```

Multi-channel:
```
[p2p~ -o 4]
[connect wss://myserver.com studio bob(
[setchannel alice 1(
```

JSON mode:
```
[p2p~ -json data]
[connect wss://myserver.com room user(
```

## Messages

- `peers` - outlet reports number of connected peers
- `setchannel user channel` - assign user to output channel (-f mode only)

## Build Requirements

- libdatachannel
- libopus
- nlohmann-json
- spdlog
- Boost (lockfree)

---

Created by Charles K. Neimog
