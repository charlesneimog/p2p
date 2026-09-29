export class Room {
    constructor(state, env) {
        this.state = state;
        this.env = env;
        this.clients = new Map();
        this.messageCounter = 0;
    }

    async fetch(request) {
        const origin = request.headers.get("Origin");
        const allowedOrigins = [
            "https://charlesneimog.github.io",
            "http://localhost:5004",
            "https://test.charlesneimog.duckdns.org",
            "https://test.charlesneimog.duckdns.org/",
            // something else
        ];

        // Allow if no Origin header OR if origin is in the allowed list
        if (!origin || allowedOrigins.includes(origin)) {
            // Accept the connection
        } else {
            return new Response("Forbidden: Unauthorized Origin", { status: 403 });
        }

        const upgrade = request.headers.get("Upgrade");
        if (!upgrade || upgrade.toLowerCase() !== "websocket") {
            return new Response("Room Durable Object");
        }

        const pair = new WebSocketPair();
        const [client, server] = Object.values(pair);
        server.accept();
        const clientId = crypto.randomUUID();
        this.clients.set(clientId, {
            id: clientId,
            ws: server,
            name: null,
            joined: false,
        });

        server.send(
            JSON.stringify({
                type: "welcome",
                id: clientId,
            }),
        );

        server.addEventListener("message", (event) => {
            this.handleMessage(clientId, event.data);
        });

        server.addEventListener("close", () => {
            this.handleClose(clientId);
        });

        return new Response(null, {
            status: 101,
            webSocket: client,
        });
    }

    handleMessage(clientId, raw) {
        const data = JSON.parse(raw);
        const peer = this.clients.get(clientId);
        if (!peer) return;

        if (data.type === "join") {
            // A socket's membership cannot change through another join message.
            if (peer.joined) return;
            const topology = data.topology ?? "mesh";
            const role = topology === "star" ? data.role : null;
            const members = [...this.clients.values()].filter((other) => other.joined);
            let error;
            if (!["mesh", "star"].includes(topology)) {
                error = "Invalid topology";
            } else if (topology === "star" && !["host", "client"].includes(role)) {
                error = "Star topology requires role: host or client";
            } else if (members.some((other) => other.topology !== topology)) {
                error = "Room topology does not match";
            } else if (topology === "star" && role === "host" && members.some((other) => other.role === "host")) {
                error = "Star room already has a host";
            }
            if (error) {
                peer.ws.send(JSON.stringify({ type: "error", message: error }));
                this.clients.delete(clientId);
                peer.ws.close(1008, error);
                return;
            }
            peer.name = data.name;
            peer.topology = topology;
            peer.role = role;
            peer.joined = true;
            const existingPeers = [];
            for (const [id, other] of this.clients) {
                if (id === clientId || !other.joined) continue;
                existingPeers.push({
                    id,
                    name: other.name,
                    topology: other.topology,
                    role: other.role,
                });
                other.ws.send(
                    JSON.stringify({
                        type: "peer-joined",
                        from: clientId,
                        peer: {
                            id: clientId,
                            name: data.name,
                            topology,
                            role,
                        },
                    }),
                );
            }
            peer.ws.send(
                JSON.stringify({
                    type: "existing-peers",
                    peers: existingPeers,
                }),
            );
            return;
        }

        if (peer.joined && data.to && data.to !== clientId) {
            const target = this.clients.get(data.to);
            if (target?.joined && target.topology === peer.topology &&
                (peer.topology === "mesh" || peer.role !== target.role)) {
                this.messageCounter++;
                target.ws.send(
                    JSON.stringify({
                        type: data.type,
                        from: clientId,
                        sdp: data.sdp,
                        candidate: data.candidate,
                        sequence_id: this.messageCounter,
                    }),
                );
            }
        }
    }

    handleClose(clientId) {
        const departed = this.clients.get(clientId);
        this.clients.delete(clientId);
        if (!departed?.joined) return;
        for (const peer of this.clients.values()) {
            if (!peer.joined) continue;
            peer.ws.send(
                JSON.stringify({
                    type: "peer-left",
                    from: clientId,
                }),
            );
        }
    }
}
