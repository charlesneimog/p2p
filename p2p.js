class SimpleP2P {
    constructor(room, name, serverUrl = "wss://p2p-signaling.charlesneimog.workers.dev") {
        this.room = room;
        this.name = name;
        this.serverUrl = serverUrl;

        this.myId = null;
        this.ws = null;
        this.peers = new Map(); // id -> { name, pc, dc }

        this.localStream = null; // Guarda o stream de áudio/vídeo local

        this.config = {
            iceServers: [{ urls: "stun:stun.l.google.com:19302" }],
        };

        // Hooks (Callbacks)
        this.onConnect = (myId) => {};
        this.onDisconnect = () => {};
        this.onPeerJoin = (peerId, peerName) => {};
        this.onPeerLeave = (peerId) => {};
        this.onMessage = (peerId, data) => {};
        this.onTrack = (peerId, remoteStream) => {}; // NOVO: Hook para receber áudio/vídeo
        this.onError = (errorMsg) => {};
        this.onLog = (msg) => {};
    }

    // --- NOVO: Método para injetar o áudio ---
    addMediaStream(stream) {
        this.localStream = stream;

        // Se já existirem conexões ativas, adiciona as faixas a elas
        for (const [id, peer] of this.peers.entries()) {
            if (peer.pc) {
                const senders = peer.pc.getSenders();
                stream.getTracks().forEach((track) => {
                    // Evita adicionar a mesma faixa duas vezes
                    const alreadyAdded = senders.find((s) => s.track === track);
                    if (!alreadyAdded) {
                        peer.pc.addTrack(track, stream);
                    }
                });
            }
        }
    }

    connect() {
        if (!this.room || !this.name) {
            this.onError("Room and Name are required.");
            return;
        }

        this.onLog(`Connecting to signaling server for room: ${this.room}`);

        const wsUrl = `${this.serverUrl}?room=${this.room}`;
        this.ws = new WebSocket(wsUrl);

        this.ws.onopen = () => {
            this.onLog("WebSocket connected.");
            this.ws.send(JSON.stringify({ type: "join", name: this.name }));
        };

        this.ws.onmessage = async (e) => {
            const msg = JSON.parse(e.data);

            switch (msg.type) {
                case "welcome":
                    this.myId = msg.id;
                    this.onConnect(this.myId);
                    break;

                case "existing-peers":
                    if (msg.peers && msg.peers.length > 0) {
                        msg.peers.forEach((p) => {
                            if (!this.peers.has(p.id)) {
                                this.peers.set(p.id, { name: p.name, pc: null, dc: null });
                            }
                        });
                    }
                    break;

                case "peer-joined":
                    if (!this.peers.has(msg.from)) {
                        this.peers.set(msg.from, { name: msg.peer.name, pc: null, dc: null });
                        this.onPeerJoin(msg.from, msg.peer.name);
                        await this._initiateConnection(msg.from);
                    }
                    break;

                case "peer-left":
                    const peer = this.peers.get(msg.from);
                    if (peer) {
                        peer.dc?.close();
                        peer.pc?.close();
                        this.peers.delete(msg.from);
                        this.onPeerLeave(msg.from);
                    }
                    break;

                case "offer":
                    await this._handleOffer(msg.from, msg.sdp);
                    break;

                case "answer":
                    await this._handleAnswer(msg.from, msg.sdp);
                    break;

                case "ice-candidate":
                    await this._handleIceCandidate(msg.from, msg.candidate);
                    break;
            }
        };

        this.ws.onerror = () => this.onError("WebSocket Error");
        this.ws.onclose = () => this.disconnect();
    }

    disconnect() {
        this.ws?.close();
        for (const [id, peer] of this.peers.entries()) {
            peer.dc?.close();
            peer.pc?.close();
        }
        this.peers.clear();
        this.myId = null;
        this.localStream = null;
        this.onDisconnect();
    }

    broadcast(jsonData) {
        const payload = JSON.stringify(jsonData);
        for (const [id, peer] of this.peers.entries()) {
            if (peer.dc?.readyState === "open") {
                peer.dc.send(payload);
            }
        }
    }

    // --- Internal WebRTC Methods ---

    async _initiateConnection(peerId) {
        const peer = this.peers.get(peerId);
        if (!peer) return;

        const pc = await this._createPeerConnection(peerId, true);
        const offer = await pc.createOffer();
        await pc.setLocalDescription(offer);

        this.ws.send(JSON.stringify({ type: "offer", to: peerId, sdp: offer }));
    }

    async _handleOffer(peerId, offer) {
        const pc = await this._createPeerConnection(peerId, false);
        await pc.setRemoteDescription(new RTCSessionDescription(offer));

        const answer = await pc.createAnswer();
        await pc.setLocalDescription(answer);

        this.ws.send(JSON.stringify({ type: "answer", to: peerId, sdp: answer }));
    }

    async _handleAnswer(peerId, answer) {
        const peer = this.peers.get(peerId);
        if (peer?.pc) {
            await peer.pc.setRemoteDescription(new RTCSessionDescription(answer));
        }
    }

    async _handleIceCandidate(peerId, candidate) {
        const peer = this.peers.get(peerId);
        if (peer?.pc) {
            await peer.pc.addIceCandidate(new RTCIceCandidate(candidate));
        }
    }

    async _createPeerConnection(peerId, isCaller) {
        let peer = this.peers.get(peerId);
        if (!peer) {
            peer = { name: "Unknown", pc: null, dc: null };
            this.peers.set(peerId, peer);
        }

        if (peer.pc) return peer.pc;

        const pc = new RTCPeerConnection(this.config);
        peer.pc = pc;

        // 1. Conexão de Dados (DataChannel)
        if (isCaller) {
            const dc = pc.createDataChannel("data");
            this._setupDataChannel(peerId, dc);
        }
        pc.ondatachannel = (event) => this._setupDataChannel(peerId, event.channel);

        // 2. Transmissão de Mídia (Local -> Remoto)
        if (this.localStream) {
            this.localStream.getTracks().forEach((track) => {
                pc.addTrack(track, this.localStream);
            });
        }

        // 3. Recepção de Mídia (Remoto -> Local)
        pc.ontrack = (event) => {
            this.onLog(`Recebendo stream de áudio/vídeo de ${peer.name}`);
            if (event.streams && event.streams[0]) {
                this.onTrack(peerId, event.streams[0]);
            } else {
                const inboundStream = new MediaStream([event.track]);
                this.onTrack(peerId, inboundStream);
            }
        };

        // 4. Tratamento Dinâmico (Renegociação se adicionar áudio no meio da chamada)
        pc.onnegotiationneeded = async () => {
            try {
                const offer = await pc.createOffer();
                await pc.setLocalDescription(offer);
                if (this.ws?.readyState === WebSocket.OPEN) {
                    this.ws.send(JSON.stringify({ type: "offer", to: peerId, sdp: pc.localDescription }));
                }
            } catch (err) {
                this.onError(`Renegotiation failed: ${err}`);
            }
        };

        pc.onicecandidate = (e) => {
            if (e.candidate && this.ws?.readyState === WebSocket.OPEN) {
                this.ws.send(JSON.stringify({ type: "ice-candidate", to: peerId, candidate: e.candidate }));
            }
        };

        return pc;
    }

    _setupDataChannel(peerId, dc) {
        const peer = this.peers.get(peerId);
        peer.dc = dc;

        dc.onmessage = (e) => {
            try {
                const data = JSON.parse(e.data);
                this.onMessage(peerId, data);
            } catch (err) {
                this.onError(`Failed to parse message from ${peer.name}`);
            }
        };
    }
}
