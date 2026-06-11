class SimpleP2P {
    constructor(room, name, serverUrl = "wss://p2p-signaling.charlesneimog.workers.dev") {
        this.room = room;
        this.name = name;
        this.serverUrl = serverUrl;
        this.myId = null;
        this.ws = null;
        this.peers = new Map();
        this.localStream = null;
        this.mediaDirections = {
            audio: SimpleP2P.Direction.SendRecv,
            video: SimpleP2P.Direction.SendRecv,
        };

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
    async addAudioStream(stream, direction = SimpleP2P.Direction.SendRecv) {
        direction = this._normalizeDirection(direction);
        this.mediaDirections.audio = direction;

        const audioTrack = stream?.getAudioTracks?.()[0] || null;
        this._mergeLocalTrack(audioTrack, direction);
        if (!audioTrack && this._directionSends(direction)) return;

        for (const [peerId, peer] of this.peers.entries()) {
            await this._addTrackToPeer(peer, audioTrack, "audio", direction);
        }
    }

    // ─────────────────────────────────────
    async addVideoStream(stream, direction = SimpleP2P.Direction.SendRecv) {
        direction = this._normalizeDirection(direction);
        this.mediaDirections.video = direction;

        const videoTrack = stream?.getVideoTracks?.()[0] || null;
        this._mergeLocalTrack(videoTrack, direction);
        if (!videoTrack && this._directionSends(direction)) return;

        for (const [peerId, peer] of this.peers.entries()) {
            await this._addTrackToPeer(peer, videoTrack, "video", direction);
        }
    }

    // ─────────────────────────────────────
    async addMediaStream(stream, options = {}) {
        const mediaOptions = typeof options === "object" && options !== null ? options : {};
        const defaultDirection = typeof options === "string" ? options : mediaOptions.direction;
        const audioDirection = mediaOptions.audioDirection || defaultDirection || SimpleP2P.Direction.SendRecv;
        const videoDirection = mediaOptions.videoDirection || defaultDirection || SimpleP2P.Direction.SendRecv;

        await this.addAudioStream(stream, audioDirection);
        await this.addVideoStream(stream, videoDirection);
    }

    // ─────────────────────────────────────
    _mergeLocalTrack(track, direction) {
        if (!track || !this._directionSends(direction)) return;

        if (!this.localStream) {
            this.localStream = new MediaStream();
        }

        const existingTrack = this.localStream.getTracks().find((localTrack) => localTrack.kind === track.kind);
        if (existingTrack) {
            this.localStream.removeTrack(existingTrack);
        }
        this.localStream.addTrack(track);
    }

    // ─────────────────────────────────────
    async _addTrackToPeer(peer, track, kind, direction = SimpleP2P.Direction.SendRecv) {
        if (!peer.pc) return;

        const transceiver = peer.pc
            .getTransceivers()
            .find((t) => t.receiver?.track?.kind === kind || t.sender?.track?.kind === kind || t.mid === kind);

        if (transceiver?.sender) {
            await transceiver.sender.replaceTrack(this._directionSends(direction) ? track : null);
            transceiver.direction = direction;
        } else if (!peer.isPolite) {
            if (track && this._directionSends(direction)) {
                peer.pc.addTransceiver(track, { direction });
            } else {
                peer.pc.addTransceiver(kind, { direction });
            }
        }
    }

    // ─────────────────────────────────────
    _normalizeDirection(direction) {
        const value = String(direction || "").toLowerCase();
        if (Object.values(SimpleP2P.Direction).includes(value)) return value;
        throw new Error(`Invalid media direction: ${direction}`);
    }

    // ─────────────────────────────────────
    _directionSends(direction) {
        return direction === SimpleP2P.Direction.SendOnly || direction === SimpleP2P.Direction.SendRecv;
    }

    // ─────────────────────────────────────
    _directionReceives(direction) {
        return direction === SimpleP2P.Direction.RecvOnly || direction === SimpleP2P.Direction.SendRecv;
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
            needsNegotiation: false,
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

        // Only the impolite side pre-creates offerable media.
        // If browser is polite, wait for Pd's offer to create media m-lines.
        if (!peer.isPolite) {
            for (const [kind, direction] of Object.entries(this.mediaDirections)) {
                const track = this.localStream?.getTracks().find((localTrack) => localTrack.kind === kind) || null;
                if (track && this._directionSends(direction)) {
                    pc.addTransceiver(track, { direction });
                } else if (direction === SimpleP2P.Direction.RecvOnly) {
                    pc.addTransceiver(kind, { direction });
                }
            }
        } else {
            // Polite peers answer the offer's m-lines instead of creating them early.
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
            this.onTrack(peerId, stream, event.track);
        };

        pc.onconnectionstatechange = () => {
            this.onLog(`Connection ${peerId}: ${pc.connectionState}`);
            if (pc.connectionState === "failed" && pc.signalingState === "stable") {
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
            if (peer.makingOffer) {
                peer.needsNegotiation = true;
                return;
            }

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
                if (peer.needsNegotiation && pc.signalingState === "stable") {
                    peer.needsNegotiation = false;
                    queueMicrotask(() => pc.onnegotiationneeded());
                }
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

            // Attach local media before answering, otherwise Firefox/Chrome can answer recvonly.
            await this._configureMediaAnswer(pc, description, "audio");
            await this._configureMediaAnswer(pc, description, "video");

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
    _mediaOfferDirection(description, kind) {
        const sdp = description?.sdp || "";
        const media = sdp.match(new RegExp(`(^|\\r?\\n)m=${kind}[\\s\\S]*?(?=\\r?\\nm=|$)`));
        if (!media) return null;

        const section = media[0];
        const direction = section.match(/\r?\na=(sendrecv|sendonly|recvonly|inactive)(\r?\n|$)/);
        return direction ? direction[1] : "sendrecv";
    }

    // ─────────────────────────────────────
    async _configureMediaAnswer(pc, offer, kind) {
        const offerDirection = this._mediaOfferDirection(offer, kind);
        if (!offerDirection || offerDirection === "inactive") return;

        const desiredDirection = this.mediaDirections[kind] || SimpleP2P.Direction.SendRecv;
        const track = this.localStream?.getTracks().find((localTrack) => localTrack.kind === kind) || null;
        const transceiver = pc
            .getTransceivers()
            .find((t) => t.receiver?.track?.kind === kind || t.sender?.track?.kind === kind);

        if (!transceiver) return;

        const answerDirection = this._answerDirection(offerDirection, desiredDirection, Boolean(track));
        if (this._directionSends(answerDirection)) {
            await transceiver.sender.replaceTrack(track);
        } else if (transceiver.sender) {
            await transceiver.sender.replaceTrack(null);
        }
        transceiver.direction = answerDirection;
    }

    // ─────────────────────────────────────
    _answerDirection(offerDirection, desiredDirection, hasSendTrack) {
        const canSend = this._directionSends(desiredDirection) && hasSendTrack;
        const canReceive = this._directionReceives(desiredDirection);
        const remoteCanReceive = offerDirection === "sendrecv" || offerDirection === "recvonly";
        const remoteCanSend = offerDirection === "sendrecv" || offerDirection === "sendonly";

        const answerSends = canSend && remoteCanReceive;
        const answerReceives = canReceive && remoteCanSend;

        if (answerSends && answerReceives) {
            return SimpleP2P.Direction.SendRecv;
        } else if (answerSends) {
            return SimpleP2P.Direction.SendOnly;
        } else if (answerReceives) {
            return SimpleP2P.Direction.RecvOnly;
        } else {
            return SimpleP2P.Direction.Inactive;
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

SimpleP2P.Direction = Object.freeze({
    SendOnly: "sendonly",
    RecvOnly: "recvonly",
    SendRecv: "sendrecv",
    Inactive: "inactive",
});
