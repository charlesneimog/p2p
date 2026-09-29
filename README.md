# p2p

`p2p` is a collection of Pure Data and Max/MSP externals for sending audio,
video, and messages directly between peers using WebRTC. A small signaling
server introduces users in the same room; media then travels peer to peer.

The collection provides:

- `p2p.config` — manages a named session and its connection
- `p2p.s.audio~` — sends an audio signal
- `p2p.r.audio~` — receives audio from a named user
- `p2p.r.video` — receives video through GEM (Pd) or Jitter (Max)

All objects belonging to one connection use the same session name as their
first argument.

## Testing

Use [https://charlesneimog.github.io/p2p/](https://charlesneimog.github.io/p2p/) to test the objects. Do not use this for your pieces, I can change this when I want! Check [`signaling-server`](signaling-server) directory to configure your server, it is easy and free!

## Compile

Requirements are CMake 3.30 or newer, a C/C++ compiler, Boost, and the Pure
Data development headers. Dependencies, including a static FFmpeg 7 build
with H.264 support, are downloaded automatically.

For Pure Data only:

```sh
cmake -S . -B build -DP2P_BUILD_MAX=OFF
cmake --build build --config Release
```

For Max/MSP only:

```sh
cmake -S . -B build-max -DP2P_BUILD_MAX=ON
cmake --build build-max --target p2p_max_package --config Release
```

If `MAX_SDK_PATH` is not set, CMake downloads Cycling '74's `max-sdk` v8.2.0.
The path may point to either a full `max-sdk` checkout or directly to its
`max-sdk-base` directory.
The Max externals are written to `build-max/p2p-max-package/externals`.

## Examples

### Pure Data

<img src="resources/pd.png" alt="Pure Data p2p patch example" width="420">

### Max/MSP

<img src="resources/max.jpeg" alt="Max/MSP p2p patch example" width="420">

## Basic use

Create the objects with a shared session name, then send the following message
to `p2p.config`:

```text
connect wss://your-server.example room-name username
```

Use `stream 1` to start sending audio and `disconnect` to leave the room. See
[`p2p-help.pd`](p2p-help.pd) for a complete Pure Data patch.

Audio sessions require the Pd or Max audio engine to run at exactly 48 kHz.
At that rate, the externals use Opus's minimum 120-sample (2.5 ms) frame size.

Both Pd and Max `p2p.config` accept these messages before `connect`:

```text
topology star
role host
connect wss://your-server.example room-name username
```

Use `role client` on each star client, or `topology mesh` for the default
full mesh behavior. Star mode requires an explicit role. Settings persist
across reconnects; send `disconnect` before changing topology or role.
Roles only determine which peers connect, and do not change media direction.
Clients wait for a host and remain in signaling when the host leaves.

## Signaling server

### Browser topologies

`p2p.js` provides `SimpleP2P` with mesh connections by default:

```js
const mesh = new SimpleP2P("room", "name");
// Equivalent: new SimpleP2P("room", "name", { topology: "mesh" });

const host = new SimpleP2P("star-room", "host", { topology: "star", role: "host" });
const client = new SimpleP2P("star-room", "client", { topology: "star", role: "client" });
host.connect();
client.connect();
```

All members of a room must use the same topology, and a star room allows only
one host. Star clients connect only to the host; they wait in signaling if no
host is present and connect when one joins, including after a host leaves.
Join rejections are reported through `onError` and close the rejected socket.
An empty room can subsequently use either topology.

`broadcast()` sends to connected peers: all other peers in mesh, all clients
for a star host, and only the host for a star client. Media directions
(`sendonly`, `recvonly`, `sendrecv`, `inactive`) work independently of topology.
Received media and messages are not automatically forwarded to other clients.
A star room with 20 devices has 19 links instead of mesh's 190.

Set `serverUrl` in the options object to use a custom signaling server. The
legacy `new SimpleP2P(room, name, serverUrl)` form remains supported as mesh.
Star mode requires the updated signaling server for browser, Pd, and Max clients.

Run the browser/signaling regression tests (with mocked WebRTC and sockets):

```sh
node --test tests/topology.test.cjs
```

A signaling server is required to establish peer connections. The included
Cloudflare Workers implementation and deployment instructions are in the
[`signaling-server`](signaling-server) directory.
