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

        if (data.type === "join") {
            const peer = this.clients.get(clientId);
            peer.name = data.name;
            const existingPeers = [];
            for (const [id, other] of this.clients) {
                if (id === clientId) continue;
                existingPeers.push({
                    id,
                    name: other.name,
                });
                other.ws.send(
                    JSON.stringify({
                        type: "peer-joined",
                        from: clientId,
                        peer: {
                            id: clientId,
                            name: data.name,
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

        if (data.to) {
            const target = this.clients.get(data.to);
            if (target) {
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
        this.clients.delete(clientId);
        for (const peer of this.clients.values()) {
            peer.ws.send(
                JSON.stringify({
                    type: "peer-left",
                    from: clientId,
                }),
            );
        }
    }
}
