class SimpleP2P {
    constructor(room, name, serverUrl = "wss://p2p-signaling.charlesneimog.workers.dev") {
        this.room = room;
        this.name = name;
        this.serverUrl = serverUrl;

        this.myId = null;
        this.ws = null;
        this.peers = new Map(); // id -> { name, pc, dc }

        this.localStream = null;

        this.config = {
            iceServers: [{ urls: "stun:stun.l.google.com:19302" }],
        };

        // Hooks (Callbacks)
        this.onConnect = (myId) => {};
        this.onDisconnect = () => {};
        this.onPeerJoin = (peerId, peerName) => {};
        this.onPeerLeave = (peerId) => {};
        this.onMessage = (peerId, data) => {};
        this.onTrack = (peerId, remoteStream) => {};
        this.onError = (errorMsg) => {};
        this.onLog = (msg) => {};
    }

    addMediaStream(stream) {
        this.localStream = stream;

        for (const [id, peer] of this.peers.entries()) {
            if (peer.pc && peer.pc.connectionState === "connected") {
                const senders = peer.pc.getSenders();
                stream.getTracks().forEach((track) => {
                    const alreadyAdded = senders.find((s) => s.track === track);
                    if (!alreadyAdded) {
                        peer.pc.addTrack(track, stream);
                        // onnegotiationneeded will fire automatically — no manual _renegotiate needed
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
                                this.peers.set(p.id, { name: p.name, pc: null, dc: null, makingOffer: false });
                            }
                        });
                    }
                    break;

                case "peer-joined":
                    if (!this.peers.has(msg.from)) {
                        this.peers.set(msg.from, { name: msg.peer.name, pc: null, dc: null, makingOffer: false });
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

    _shouldBeCaller(peerId) {
        const sortedIds = [this.myId, peerId].sort();
        return sortedIds[0] === this.myId;
    }

    // ─────────────────────────────────────
    async _initiateConnection(peerId) {
        const peer = this.peers.get(peerId);
        if (!peer) return;

        if (!this._shouldBeCaller(peerId)) {
            this.onLog(`Waiting for ${peerId} to initiate connection`);
            return;
        }

        this.onLog(`Initiating connection as caller to ${peerId}`);
        // _createPeerConnection sets up onnegotiationneeded which will send the first offer.
        // We do NOT manually create/send an offer here to avoid the double-offer race.
        await this._createPeerConnection(peerId, true);
    }

    async _handleOffer(peerId, offer) {
        let peer = this.peers.get(peerId);

        // FIX: If the peer isn't in our map yet (race condition), add them.
        if (!peer) {
            peer = { name: "Unknown", pc: null, dc: null, makingOffer: false };
            this.peers.set(peerId, peer);
        }

        const polite = !this._shouldBeCaller(peerId); // answerer = polite peer

        const offerCollision = peer.makingOffer || (peer.pc && peer.pc.signalingState !== "stable");

        if (!polite && offerCollision) {
            // Impolite peer ignores colliding offers
            this.onLog(`Ignoring colliding offer from ${peerId} (impolite peer)`);
            return;
        }

        // FIX: Re-use the existing RTCPeerConnection for renegotiation offers
        //      instead of creating a new one. _createPeerConnection returns the
        //      existing pc if peer.pc is already set.
        const pc = await this._createPeerConnection(peerId, false);

        // FIX: If there's a collision and we're the polite peer, roll back first.
        if (offerCollision) {
            await Promise.all([
                pc.setLocalDescription({ type: "rollback" }),
                pc.setRemoteDescription(new RTCSessionDescription(offer)),
            ]);
        } else {
            await pc.setRemoteDescription(new RTCSessionDescription(offer));
        }

        const answer = await pc.createAnswer();
        await pc.setLocalDescription(answer);

        this.ws.send(
            JSON.stringify({
                type: "answer",
                to: peerId,
                sdp: answer,
            }),
        );
    }

    async _handleAnswer(peerId, answer) {
        const peer = this.peers.get(peerId);
        if (peer?.pc) {
            // Guard: only apply if we're actually waiting for an answer
            if (peer.pc.signalingState === "have-local-offer") {
                await peer.pc.setRemoteDescription(new RTCSessionDescription(answer));
            } else {
                this.onLog(`Ignoring late/stale answer from ${peerId} (signalingState: ${peer.pc.signalingState})`);
            }
        }
    }

    async _handleIceCandidate(peerId, candidate) {
        const peer = this.peers.get(peerId);
        if (!peer) {
            this.onLog(`Queueing ICE candidate from unknown peer ${peerId}`);
            this.peers.set(peerId, {
                name: "Unknown",
                pc: null,
                dc: null,
                makingOffer: false,
                pendingCandidates: [candidate],
            });
            return;
        }

        if (!peer.pc || !peer.pc.remoteDescription) {
            peer.pendingCandidates ??= [];
            peer.pendingCandidates.push(candidate);
            this.onLog(`Queueing ICE candidate from ${peerId} (remote description not set yet)`);
            return;
        }

        try {
            await peer.pc.addIceCandidate(new RTCIceCandidate(candidate));
        } catch (e) {
            this.onError(`ICE candidate error for ${peerId}: ${e}`);
        }
    }

    async _createPeerConnection(peerId, isCaller) {
        let peer = this.peers.get(peerId);
        if (!peer) {
            peer = { name: "Unknown", pc: null, dc: null, makingOffer: false };
            this.peers.set(peerId, peer);
        }

        // FIX: Return existing PC — this is intentional for renegotiation.
        if (peer.pc) return peer.pc;

        const pc = new RTCPeerConnection(this.config);
        peer.pc = pc;

        // 1. DataChannel
        if (isCaller) {
            const dc = pc.createDataChannel("data");
            this._setupDataChannel(peerId, dc);
        }
        pc.ondatachannel = (event) => {
            this._setupDataChannel(peerId, event.channel);
        };

        // 2. Local media tracks (if already available)
        if (this.localStream) {
            this.localStream.getTracks().forEach((track) => {
                pc.addTrack(track, this.localStream);
            });
        }

        // 3. Receive remote media
        pc.ontrack = (event) => {
            this.onLog(`Receiving audio/video track from ${peer.name}`);
            const stream = event.streams?.[0] ?? new MediaStream([event.track]);
            this.onTrack(peerId, stream);
        };

        // 4. FIX: Use makingOffer flag (perfect negotiation pattern) to prevent
        //    onnegotiationneeded from sending a new offer while one is already in flight.
        pc.onnegotiationneeded = async () => {
            if (peer.makingOffer) return;
            try {
                peer.makingOffer = true;
                await pc.setLocalDescription(); // browser picks offer/rollback automatically
                if (this.ws?.readyState === WebSocket.OPEN) {
                    this.ws.send(JSON.stringify({ type: "offer", to: peerId, sdp: pc.localDescription }));
                }
            } catch (err) {
                this.onError(`Negotiation failed: ${err}`);
            } finally {
                peer.makingOffer = false;
            }
        };

        pc.onicecandidate = (e) => {
            if (e.candidate && this.ws?.readyState === WebSocket.OPEN) {
                this.ws.send(JSON.stringify({ type: "ice-candidate", to: peerId, candidate: e.candidate }));
            }
        };

        pc.onconnectionstatechange = () => {
            this.onLog(`PC state with ${peerId}: ${pc.connectionState}`);
            if (pc.connectionState === "failed") {
                this.onError(`Connection to ${peerId} failed`);
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
