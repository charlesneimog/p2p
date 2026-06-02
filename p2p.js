class SimpleP2P {
    // 1. Added serverUrl with a default fallback
    constructor(room, name, serverUrl = "wss://p2p-signaling.charlesneimog.workers.dev") {
        this.room = room;
        this.name = name;
        this.serverUrl = serverUrl; // Store the URL

        this.myId = null;
        this.ws = null;
        this.peers = new Map(); // id -> { name, pc, dc }

        this.config = {
            iceServers: [{ urls: "stun:stun.l.google.com:19302" }],
        };

        // Hooks (Callbacks)
        this.onConnect = (myId) => {};
        this.onDisconnect = () => {};
        this.onPeerJoin = (peerId, peerName) => {};
        this.onPeerLeave = (peerId) => {};
        this.onMessage = (peerId, data) => {};
        this.onError = (errorMsg) => {};
        this.onLog = (msg) => {};
    }

    connect() {
        if (!this.room || !this.name) {
            this.onError("Room and Name are required.");
            return;
        }

        this.onLog(`Connecting to signaling server for room: ${this.room}`);

        // 2. Build the URL dynamically based on the constructor parameter
        const wsUrl = `${this.serverUrl}?room=${this.room}`;
        this.ws = new WebSocket(wsUrl);

        this.ws.onopen = () => {
            this.onLog("WebSocket connected.");
            this.ws.send(JSON.stringify({ type: "join", name: this.name }));
        };

        this.ws.onmessage = async (e) => {
            const msg = JSON.parse(e.data);
            this.onLog(`Signaling message received: ${msg.type}`);

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

        if (isCaller) {
            const dc = pc.createDataChannel("data");
            this._setupDataChannel(peerId, dc);
        }

        pc.ondatachannel = (event) => {
            this._setupDataChannel(peerId, event.channel);
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

        dc.onopen = () => this.onLog(`DataChannel open with ${peer.name}`);
        dc.onclose = () => this.onLog(`DataChannel closed with ${peer.name}`);

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
