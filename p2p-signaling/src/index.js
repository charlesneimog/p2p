// src/index.js (mesmo código, nenhuma mudança necessária)
export default {
    async fetch(request, env) {
        const url = new URL(request.url);
        const roomName = url.searchParams.get("room");

        if (request.method === "GET" && (!roomName || url.pathname === "/")) {
            return new Response("✅ Signaling Server Online\nUse: wss://p2p.charlesneimog.workers.dev?room=NOME", {
                status: 200,
                headers: { "Content-Type": "text/plain" },
            });
        }

        if (!roomName) {
            return new Response("Missing ?room=NAME parameter", { status: 400 });
        }

        const upgradeHeader = request.headers.get("Upgrade");
        if (!upgradeHeader || upgradeHeader !== "websocket") {
            return new Response("WebSocket connections only", { status: 426 });
        }

        if (!env.WEBSOCKET_SERVER) {
            console.error("WEBSOCKET_SERVER binding not found");
            return new Response("Configuration error", { status: 500 });
        }

        const id = env.WEBSOCKET_SERVER.idFromName(roomName);
        const stub = env.WEBSOCKET_SERVER.get(id);

        return stub.fetch(request);
    },
};

export class WebSocketServer {
    constructor(state, env) {
        this.state = state;
        this.sessions = new Map();
    }

    async fetch(request) {
        const pair = new WebSocketPair();
        const [client, server] = Object.values(pair);

        this.state.acceptWebSocket(server);
        const wsId = crypto.randomUUID();

        this.sessions.set(wsId, { ws: server, name: null });

        server.addEventListener("message", (event) => {
            try {
                const msg = JSON.parse(event.data);

                if (msg.type === "join") {
                    const session = this.sessions.get(wsId);
                    session.name = msg.name || `User_${wsId.slice(0, 4)}`;

                    server.send(
                        JSON.stringify({
                            type: "welcome",
                            id: wsId,
                        }),
                    );

                    for (const [id, s] of this.sessions.entries()) {
                        if (id !== wsId && s.ws.readyState === 1) {
                            s.ws.send(
                                JSON.stringify({
                                    type: "peer-joined",
                                    from: wsId,
                                    peer: { id: wsId, name: session.name },
                                }),
                            );
                        }
                    }
                }

                if (msg.type === "offer" || msg.type === "answer" || msg.type === "ice-candidate") {
                    const target = this.sessions.get(msg.to);
                    if (target && target.ws.readyState === 1) {
                        target.ws.send(
                            JSON.stringify({
                                type: msg.type,
                                from: wsId,
                                sdp: msg.sdp,
                                candidate: msg.candidate,
                            }),
                        );
                    }
                }
            } catch (err) {
                console.error("Error:", err);
            }
        });

        server.addEventListener("close", () => {
            for (const [id, s] of this.sessions.entries()) {
                if (id !== wsId && s.ws.readyState === 1) {
                    s.ws.send(
                        JSON.stringify({
                            type: "peer-left",
                            from: wsId,
                        }),
                    );
                }
            }
            this.sessions.delete(wsId);
        });

        return new Response(null, { status: 101, webSocket: client });
    }
}
