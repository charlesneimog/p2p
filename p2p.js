class SimpleP2P {
    constructor(room, name, serverUrl = "wss://p2p-signaling.charlesneimog.workers.dev") {
        this.room = room;
        this.name = name;
        this.serverUrl = serverUrl;
        this.myId = null;
        this.ws = null;
        this.peers = new Map();
        this.localStream = null;

        this.config = {
            iceServers: [{ urls: "stun:stun.l.google.com:19302" }],
        };

        this.onConnect = () => {};
        this.onDisconnect = () => {};
        this.onPeerJoin = () => {};
        this.onPeerLeave = () => {};
        this.onMessage = () => {};
        this.onTrack = () => {};
        this.onError = () => {};
        this.onLog = () => {};
    }

    // ─────────────────────────────────────
    connect() {
        const url = `${this.serverUrl}?room=${this.room}`;
        this.ws = new WebSocket(url);

        this.ws.onopen = () => {
            this.onLog("WebSocket connected");
            this.ws.send(JSON.stringify({ type: "join", name: this.name }));
        };

        this.ws.onmessage = async (event) => {
            const msg = JSON.parse(event.data);

            switch (msg.type) {
                case "welcome":
                    this.myId = msg.id;
                    this.onConnect(msg.id);
                    break;

                case "existing-peers":
                    for (const peer of msg.peers) {
                        await this._ensurePeer(peer.id, peer.name);
                    }
                    break;

                case "peer-joined":
                    await this._ensurePeer(msg.from, msg.peer.name);
                    this.onPeerJoin(msg.from, msg.peer.name);
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

                case "peer-left":
                    this._removePeer(msg.from);
                    break;
            }
        };

        this.ws.onclose = () => this.disconnect();
        this.ws.onerror = (err) => this.onError(err);
    }

    // ─────────────────────────────────────
    disconnect() {
        for (const [id, peer] of this.peers.entries()) {
            peer.pc?.close();
            peer.dc?.close();
        }
        this.peers.clear();
        if (this.ws) this.ws.close();
        this.onDisconnect();
    }

    // ─────────────────────────────────────
    async addMediaStream(stream) {
        this.localStream = stream;
        const audioTrack = stream.getAudioTracks()[0];
        if (!audioTrack) return;

        // Add track to all existing peers
        for (const [peerId, peer] of this.peers.entries()) {
            await this._addTrackToPeer(peer, audioTrack);
        }
    }

    // ─────────────────────────────────────
    async _addTrackToPeer(peer, audioTrack) {
        if (!peer.pc) return;

        const transceiver = peer.pc
            .getTransceivers()
            .find((t) => t.receiver?.track?.kind === "audio" || t.mid === "audio");

        if (transceiver?.sender) {
            transceiver.direction = "sendrecv";
            await transceiver.sender.replaceTrack(audioTrack);
        } else if (!peer.isPolite) {
            const newTransceiver = peer.pc.addTransceiver(audioTrack, {
                direction: "sendrecv",
            });
            // Trigger renegotiation
            if (peer.pc.signalingState === "stable") {
                peer.pc.onnegotiationneeded();
            }
        }
    }
    // ─────────────────────────────────────
    broadcast(data) {
        const payload = JSON.stringify(data);
        for (const [id, peer] of this.peers.entries()) {
            if (peer.dc?.readyState === "open") {
                peer.dc.send(payload);
            }
        }
    }

    // ─────────────────────────────────────
    async _ensurePeer(peerId, peerName = "Unknown") {
        if (this.peers.has(peerId)) return this.peers.get(peerId);

        const peer = {
            id: peerId,
            name: peerName,
            isPolite: this._determinePoliteness(this.myId, peerId),
            pc: null,
            dc: null,
            makingOffer: false,
            ignoreOffer: false,
            pendingCandidates: [],
        };

        this.peers.set(peerId, peer);
        peer.pc = this._createPeerConnection(peerId, peer);

        return peer;
    }

    // ─────────────────────────────────────
    // Single source of truth for topology and politeness
    _determinePoliteness(myId, remoteId) {
        return myId > remoteId;
    }

    // ─────────────────────────────────────
    _createPeerConnection(peerId, peer) {
        const pc = new RTCPeerConnection(this.config);

        let audioTransceiver = null;

        // Only the impolite side pre-creates offerable media.
        // If browser is polite, wait for Pd's offer to create the audio m-line.
        if (this.localStream) {
            const audioTrack = this.localStream.getAudioTracks()[0];
            if (audioTrack && !peer.isPolite) {
                const transceiver = pc.addTransceiver(audioTrack, {
                    direction: "sendrecv",
                });
            } else if (audioTrack) {
                // For polite peers, just create a recvonly transceiver initially
                pc.addTransceiver("audio", { direction: "recvonly" });
            }
        } else if (!peer.isPolite) {
            // No media yet, but create placeholder
            pc.addTransceiver("audio", { direction: "sendrecv" });
        }

        pc.onicecandidate = (event) => {
            if (!event.candidate) return;

            this.ws.send(
                JSON.stringify({
                    type: "ice-candidate",
                    to: peerId,
                    candidate: event.candidate,
                }),
            );
        };

        pc.ontrack = (event) => {
            const stream = event.streams[0] || new MediaStream([event.track]);
            this.onTrack(peerId, stream);
        };

        pc.onconnectionstatechange = () => {
            this.onLog(`Connection ${peerId}: ${pc.connectionState}`);
            if (pc.connectionState === "failed") {
                pc.restartIce();
            }
        };

        if (!peer.isPolite) {
            const dc = pc.createDataChannel("data");
            this._setupDataChannel(peerId, dc);
        }

        pc.ondatachannel = (event) => {
            this._setupDataChannel(peerId, event.channel);
        };

        pc.onnegotiationneeded = async () => {
            if (pc.signalingState !== "stable") return;

            try {
                peer.makingOffer = true;

                await pc.setLocalDescription();

                this.ws.send(
                    JSON.stringify({
                        type: "offer",
                        to: peerId,
                        sdp: pc.localDescription,
                    }),
                );
            } catch (err) {
                this.onError(new Error(`Negotiation failed: ${err.message}`));
            } finally {
                peer.makingOffer = false;
            }
        };

        return pc;
    }

    // ─────────────────────────────────────
    async _handleOffer(peerId, description) {
        const peer = this.peers.get(peerId);
        if (!peer || !peer.pc) return;

        const pc = peer.pc;
        const offerCollision = peer.makingOffer || pc.signalingState !== "stable";

        peer.ignoreOffer = !peer.isPolite && offerCollision;

        if (peer.ignoreOffer) {
            this.onLog(`Collision detected. As the impolite peer, dropping offer from ${peerId}.`);
            return;
        }

        try {
            await pc.setRemoteDescription(new RTCSessionDescription(description));

            // IMPORTANT: browser is polite/callee here.
            // Pd's offer created the audio transceiver.
            // Attach mic before creating the answer, otherwise Firefox/Chrome answers recvonly.
            if (this.localStream) {
                const audioTrack = this.localStream.getAudioTracks()[0];

                const audioTransceiver = pc
                    .getTransceivers()
                    .find((t) => t.receiver?.track?.kind === "audio" || t.mid === "audio");

                if (audioTransceiver) {
                    audioTransceiver.direction = "sendrecv";

                    const audioTrack = this.localStream?.getAudioTracks()[0];
                    if (audioTrack) {
                        await audioTransceiver.sender.replaceTrack(audioTrack);
                    }
                }
            }

            await this._flushPendingCandidates(peer);
            await pc.setLocalDescription();
            this.ws.send(
                JSON.stringify({
                    type: "answer",
                    to: peerId,
                    sdp: pc.localDescription,
                }),
            );
        } catch (err) {
            this.onError(new Error(`Failed to handle offer: ${err.message}`));
        }
    }

    // ─────────────────────────────────────
    async _handleAnswer(peerId, answer) {
        const peer = this.peers.get(peerId);
        if (!peer || !peer.pc) return;

        try {
            if (peer.pc.signalingState !== "have-local-offer") return;

            await peer.pc.setRemoteDescription(new RTCSessionDescription(answer));
            await this._flushPendingCandidates(peer);
        } catch (err) {
            this.onError(err);
        }
    }

    // ─────────────────────────────────────
    async _flushPendingCandidates(peer) {
        while (peer.pendingCandidates.length > 0) {
            const candidate = peer.pendingCandidates.shift();
            try {
                await peer.pc.addIceCandidate(new RTCIceCandidate(candidate));
            } catch (e) {
                this.onLog(`Failed to add queued ICE candidate: ${e.message}`);
            }
        }
    }

    // ─────────────────────────────────────
    async _handleIceCandidate(peerId, candidate) {
        const peer = this.peers.get(peerId);
        if (!peer || !peer.pc) return;

        try {
            if (!peer.pc.remoteDescription) {
                if (!peer.ignoreOffer) {
                    peer.pendingCandidates.push(candidate);
                }
                return;
            }
            await peer.pc.addIceCandidate(new RTCIceCandidate(candidate));
        } catch (err) {
            if (!peer.ignoreOffer) {
                this.onError(err);
            }
        }
    }

    // ─────────────────────────────────────
    _setupDataChannel(peerId, dc) {
        const peer = this.peers.get(peerId);
        if (!peer) return;

        peer.dc = dc;
        dc.onopen = () => this.onLog(`DataChannel open with ${peerId}`);
        dc.onclose = () => this.onLog(`DataChannel closed with ${peerId}`);
        dc.onmessage = (event) => {
            try {
                this.onMessage(peerId, JSON.parse(event.data));
            } catch {
                this.onMessage(peerId, event.data);
            }
        };
    }

    // ─────────────────────────────────────
    _removePeer(peerId) {
        const peer = this.peers.get(peerId);
        if (!peer) return;

        peer.dc?.close();
        peer.pc?.close();
        this.peers.delete(peerId);
        this.onPeerLeave(peerId);
    }
}
