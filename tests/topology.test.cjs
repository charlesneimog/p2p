const assert = require("node:assert/strict");
const { readFileSync } = require("node:fs");
const { resolve } = require("node:path");
const { runInNewContext } = require("node:vm");
const { test } = require("node:test");

const source = (path) => readFileSync(resolve(__dirname, "..", path), "utf8");
const Room = runInNewContext(`${source("signaling-server/src/room.js").replace("export class", "class")}\nRoom`);

function harness() {
    const room = new Room();
    const pending = [];
    const pcs = [];
    let nextId = 0;
    class WebSocket {
        constructor() {
            this.id = String(++nextId).padStart(3, "0");
            this.closed = false;
            this.received = [];
            room.clients.set(this.id, {
                id: this.id, joined: false, name: null,
                ws: {
                    send: (raw) => {
                        this.received.push(JSON.parse(raw));
                        pending.push(() => this.onmessage({ data: raw }));
                    },
                    close: () => this.close(),
                },
            });
            pending.push(() => this.onmessage({ data: JSON.stringify({ type: "welcome", id: this.id }) }));
            pending.push(() => this.onopen());
        }
        send(raw) { room.handleMessage(this.id, raw); }
        close() {
            if (this.closed) return;
            this.closed = true;
            room.handleClose(this.id);
        }
    }
    class RTCPeerConnection {
        constructor(config) {
            this.config = config;
            this.transceivers = [];
            this.channels = [];
            pcs.push(this);
        }
        createDataChannel() {
            const dc = { readyState: "open", sent: [], send(data) { this.sent.push(data); }, close() { this.readyState = "closed"; } };
            this.channels.push(dc);
            return dc;
        }
        addTransceiver(track, { direction }) {
            const sender = { track: typeof track === "string" ? null : track, async replaceTrack(value) { this.track = value; } };
            this.transceivers.push({ sender, receiver: { track: { kind: track.kind || track } }, direction });
        }
        getTransceivers() { return this.transceivers; }
        close() { this.closed = true; }
    }
    const SimpleP2P = runInNewContext(`${source("p2p.js")}\nSimpleP2P`, { WebSocket, RTCPeerConnection });
    function join(options) {
        const client = new SimpleP2P("room", "name", options);
        client.errors = [];
        client.onError = (error) => client.errors.push(error.message);
        client.connect();
        return client;
    }
    async function flush() { while (pending.length) await pending.shift()(); }
    return { room, pcs, SimpleP2P, join, flush };
}

test("legacy constructor and metadata-free joins remain mesh", async () => {
    const h = harness();
    assert.equal(new h.SimpleP2P("r", "n", "ws://custom").serverUrl, "ws://custom");
    const clients = Array.from({ length: 20 }, () => h.join());
    await h.flush();
    assert.equal(h.pcs.length, 380); // Two browser endpoints for each of 190 links.
    for (const client of clients) assert.equal(client.peers.size, 19);
    assert.equal(clients[0]._shouldConnectTo({ id: "legacy" }), true);
    const legacy = { joined: false, ws: { send() {} } };
    h.room.clients.set("legacy", legacy);
    h.room.handleMessage("legacy", JSON.stringify({ type: "join", name: "old" }));
    assert.equal(legacy.topology, "mesh");
});

test("clients wait for a host, form 19 links, and reconnect to a replacement host", async () => {
    const h = harness();
    const clients = Array.from({ length: 19 }, () => h.join({ topology: "star", role: "client" }));
    await h.flush();
    assert.equal(h.pcs.length, 0);
    const host = h.join({ topology: "star", role: "host" });
    await h.flush();
    assert.equal(h.pcs.length, 38); // Two endpoints per link, 19 links total.
    assert.equal(host.peers.size, 19);
    const advertised = host.ws.received.find((message) => message.type === "existing-peers").peers;
    assert.ok(advertised.every((peer) => peer.topology === "star" && peer.role === "client"));
    assert.equal(clients[0].ws.received.find((message) => message.type === "peer-joined" && message.from === host.myId).peer.role, "host");
    for (const client of clients) {
        assert.deepEqual([...client.peers.keys()], [host.myId]);
        client.broadcast("to host");
        assert.equal(client.peers.get(host.myId).dc.sent.length, 1);
    }
    const before = clients[1].ws.received.length;
    for (const type of ["offer", "answer", "ice-candidate"]) {
        clients[0].ws.send(JSON.stringify({ type, to: clients[1].myId }));
        // Even unexpected signaling arriving directly cannot create a peer.
        await clients[1].ws.onmessage({ data: JSON.stringify({ type, from: clients[0].myId }) });
    }
    assert.equal(clients[1].ws.received.length, before);
    assert.equal(h.pcs.length, 38);
    clients[0].ws.send(JSON.stringify({ type: "ice-candidate", to: host.myId, candidate: { candidate: "test" } }));
    await h.flush();
    assert.equal(host.peers.get(clients[0].myId).pendingCandidates.length, 1);
    host.disconnect();
    await h.flush();
    for (const client of clients) {
        assert.equal(client.peers.size, 0);
        assert.equal(client.ws.closed, false);
    }
    assert.ok(h.pcs.every((pc) => pc.closed));
    const replacement = h.join({ topology: "star", role: "host" });
    await h.flush();
    assert.equal(replacement.peers.size, 19);
    for (const client of clients) assert.deepEqual([...client.peers.keys()], [replacement.myId]);
});

test("host-first discovery, broadcast, media directions, and no media forwarding", async () => {
    const h = harness();
    const host = h.join({ topology: "star", role: "host" });
    await h.flush();
    const client = h.join({ topology: "star", role: "client" });
    const other = h.join({ topology: "star", role: "client" });
    await h.flush();
    host.broadcast({ hello: true });
    for (const peer of host.peers.values()) assert.equal(peer.dc.sent.length, 1);
    assert.equal(client.peers.size, 1);
    assert.equal(other.peers.size, 1);
    for (const endpoint of [host, client]) {
        const pc = [...endpoint.peers.values()][0].pc;
        pc.addTransceiver("audio", { direction: "sendrecv" });
        const track = { kind: "audio" };
        endpoint.localStream = { getTracks: () => [track] };
        for (const direction of ["sendonly", "recvonly", "sendrecv", "inactive"]) {
            endpoint.mediaDirections.audio = direction;
            await endpoint._configureMediaAnswer(pc, { sdp: "m=audio 9 RTP/AVP 0\r\na=sendrecv\r\n" }, "audio");
            assert.equal(pc.transceivers[0].direction, direction);
            assert.equal(pc.transceivers[0].sender.track, direction.includes("send") ? track : null);
        }
    }
    const remoteTrack = { kind: "audio" };
    const stream = { getTracks: () => [remoteTrack] };
    let received;
    host.onTrack = (...args) => { received = args; };
    host.peers.get(client.myId).pc.ontrack({ streams: [stream], track: remoteTrack });
    assert.equal(received[1], stream);
    assert.equal(host.localStream.getTracks().includes(remoteTrack), false);
    assert.equal(host.peers.get(other.myId).pc.transceivers.length, 0);
});

test("reject incompatible joins without disturbing membership", async () => {
    const h = harness();
    const host = h.join({ topology: "star", role: "host" });
    await h.flush();
    const secondHost = h.join({ topology: "star", role: "host" });
    const mesh = h.join();
    await h.flush();
    for (const rejected of [secondHost, mesh]) {
        assert.equal(rejected.errors.length, 1);
        assert.equal(rejected.ws.closed, true);
    }
    assert.equal(h.room.clients.size, 1);
    assert.equal(host.peers.size, 0);
    host.ws.send(JSON.stringify({ type: "join", topology: "mesh" }));
    assert.equal(h.room.clients.get(host.myId).topology, "star");
    host.disconnect();
    const newMesh = h.join();
    await h.flush();
    const star = h.join({ topology: "star", role: "client" });
    await h.flush();
    assert.equal(star.errors.length, 1);
    assert.equal(newMesh.peers.size, 0);
    assert.throws(() => h.join({ topology: "invalid" }), /Invalid topology/);
    assert.throws(() => h.join({ topology: "star" }), /requires role/);
});

test("unjoined sockets are neither advertised nor allowed to signal", async () => {
    const h = harness();
    const peer = h.join();
    const unjoined = h.join();
    unjoined.ws.onopen = () => {};
    await h.flush();
    assert.equal(peer.peers.size, 0);
    const before = peer.ws.received.length;
    unjoined.ws.send(JSON.stringify({ type: "offer", to: peer.myId }));
    unjoined.ws.close();
    await h.flush();
    assert.equal(peer.ws.received.length, before);
});

test("server validates topology and role independently of the browser", () => {
    const h = harness();
    for (const metadata of [{ topology: "invalid" }, { topology: "star" }, { topology: "star", role: "invalid" }]) {
        const messages = [];
        let closeCode;
        h.room.clients.set("invalid", { joined: false, ws: {
            send: (raw) => messages.push(JSON.parse(raw)),
            close: (code) => { closeCode = code; },
        } });
        h.room.handleMessage("invalid", JSON.stringify({ type: "join", ...metadata }));
        assert.equal(messages[0].type, "error");
        assert.equal(closeCode, 1008);
        assert.equal(h.room.clients.size, 0);
    }
});
