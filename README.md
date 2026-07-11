# p2p~

## Receiving video in GEM

When built with GEM and FFmpeg support, the Pure Data object accepts GEM's render list and
places the latest received WebRTC video frame on it. The receiver currently negotiates H.264.

```text
[gemhead]
|
[p2p~ -v]
|
[pix_texture]
|
[rectangle 5.33 3]
```

Video reception and its GEM outlet are created only when `[p2p~]` has the `-v` flag. GEM's
source is fetched automatically with CPM. CMake enables the feature when it also finds
the `libavcodec`, `libavutil`, and `libswscale` pkg-config modules. Load GEM before creating
`[p2p~]`, because the external uses GEM's `pixBlock` and `GemState` ABI. To build without the
video integration, configure with `-DP2P_GEM_VIDEO=OFF`.

A Pure Data external for peer-to-peer audio streaming using WebRTC. 

## Basic Usage

<img src="resources/help.png" width="600">

## Server

You need to create your own server (cloudflare offers a free one) to run this. Check the `signaling-server` folder.

## Connection Flow

1. Create `[p2p~]`;
2. Send `connect URL room username`;
3. Turn on stream;
4. Output shows peer count via `peers` message.

## Outputs

- Left outlet: audio signals (multichannel)
- Right outlet: messages (`peers`, `json` data)

## Options

| Flag | Description |
|------|-------------|
| `-o N` | Multi-channel mode with N outputs |
| `-f` | Fixed channel mapping with `setchannel` |
| `-json key` | Parse incoming JSON messages |

## Messages

- `peers` - outlet reports number of connected peers
- `setchannel user channel` - assign user to output channel (-f mode only)
