# p2p object split testing

Build and install the external, then start Pd at 48 kHz and open `p2p-test.pd`.
Replace the signalling URL, room, and local username in the `connect` message.

1. Connect with no remote peer. The config outlet should print `connected` and
   `connections 0`.
2. Join as `alice` from another client. Expect `peer joined alice`, followed by
   `connections 1` when the PeerConnection reaches `Connected`.
3. Start DSP and enable `stream 1`. A one-channel signal is sent as mono. Connect
   a two-channel multichannel signal and confirm that distinct left and right
   inputs arrive as stereo. Both are sent to all current peers and to peers which
   join later.
4. Disconnect and reconnect Alice under the same username. The receive outlet
   must be silent between connections and resume without recreating the object.
5. With GEM available, create the video receiver before connecting. Until an
   H264 frame is decoded, its incoming GEM state is passed through unchanged.
6. Add a second remote peer and remove peers in turn. The count should follow
   `0, 1, 2, 1, 0`.
7. While connected, delete and recreate each media object. The config session
   must remain connected. Creating a duplicate sender or duplicate receiver for
   `test, alice` must emit an error and leave the first object active.
8. Delete `p2p.config` while DSP and GEM are active. Sender activity must stop,
   audio must become silent, and video must pass through without a crash.

For cleanup/thread checks, repeat steps 3-8 under AddressSanitizer and
ThreadSanitizer builds where the platform supports libdatachannel under those
sanitizers.

## TLS trust smoke test

Run the live signalling smoke test with:

```sh
pd -noprefs -nogui -stderr -path build -open puredata/p2p-tls-smoke.pd
```

Expect `connected` and `connections 0`. To validate the explicit override
error, run it with an invalid path:

```sh
SSL_CERT_FILE=/does/not/exist pd -noprefs -nogui -stderr -path build \
  -open puredata/p2p-tls-smoke.pd
```

The config object must report the invalid `SSL_CERT_FILE` path and must not
disable certificate verification.
