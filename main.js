(() => {
    "use strict";
    const roomInput = document.getElementById("room-name");
    const nameInput = document.getElementById("user-name");
    const connectionForm = document.getElementById("connection-form");
    const connectButton = document.getElementById("btn-connect");
    const disconnectButton = document.getElementById("btn-disconnect");
    const statusBar = document.getElementById("status-bar");
    const statusText = document.getElementById("status-text");
    const peersList = document.getElementById("peer-list");
    const peerCount = document.getElementById("peer-count");
    const log = document.getElementById("console-log");
    const sendMessageButton = document.getElementById("btn-test-msg");
    const artButton = document.getElementById("video-test-art");
    const localVideo = document.getElementById("local-video");
    const remoteVideos = document.getElementById("remote-videos");
    nameInput.value = `user_${Math.floor(Math.random() * 10000).toString().padStart(4, "0")}`;

    let network = null;
    let uiRefreshInterval = null;

    function addLog(message, type = "info") {
        if (log.firstElementChild?.textContent === "Waiting for a connection.") log.replaceChildren();
        const entry = document.createElement("div");
        entry.className = "log-entry";
        entry.dataset.type = type;
        entry.textContent = `[${new Date().toLocaleTimeString("en-US", { hour12: false })}] ${message}`;
        log.appendChild(entry);
        log.scrollTop = log.scrollHeight;
    }

    function updateStatus(text, state) {
        statusText.textContent = text;
        statusBar.dataset.state = state;
    }

    function renderPeers() {
        peersList.replaceChildren();
        if (!network || network.peers.size === 0) {
            const empty = document.createElement("p");
            empty.className = "empty-state";
            empty.textContent = "No peers connected";
            peersList.appendChild(empty);
            peerCount.textContent = "0 connected";
            return;
        }
        peerCount.textContent = `${network.peers.size} connected`;
        for (const peer of network.peers.values()) {
            const item = document.createElement("span");
            item.className = "peer-item";
            item.dataset.online = String(peer.dc?.readyState === "open");
            item.textContent = peer.name;
            peersList.appendChild(item);
        }
    }

    function ensureRemoteVideo(peerId) {
        let video = document.getElementById(`video-${peerId}`);
        if (!video) {
            video = document.createElement("video");
            video.id = `video-${peerId}`;
            video.autoplay = true;
            video.playsInline = true;
            video.controls = true;
            remoteVideos.appendChild(video);
        }
        return video;
    }

    connectionForm.addEventListener("submit", (event) => {
        event.preventDefault();
        const room = roomInput.value.trim();
        const name = nameInput.value.trim();
        if (!room || !name) {
            addLog("Room name and user name are required.", "error");
            return;
        }
        updateStatus("Connecting…", "connecting");
        connectButton.disabled = true;
        if (typeof SimpleP2P === "undefined") {
            addLog("SimpleP2P could not be loaded.", "error");
            updateStatus("Disconnected", "disconnected");
            connectButton.disabled = false;
            return;
        }

        network = new SimpleP2P(room, name);
        network.onLog = (message) => addLog(message);
        network.onError = (error) => addLog(`System error: ${error?.message || error}`, "error");
        network.onConnect = async (myId) => {
            updateStatus(`Connected as ${name}`, "connected");
            disconnectButton.disabled = false;
            sendMessageButton.disabled = false;
            addLog(`Signaling server connected. ID: ${myId.substring(0, 8)}`, "success");
            renderPeers();
            uiRefreshInterval = window.setInterval(renderPeers, 1000);
            try {
                const stream = await navigator.mediaDevices.getUserMedia({
                    audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false, channelCount: 1 },
                    video: { width: { ideal: 1900 }, height: { ideal: 1080 }, facingMode: "environment" },
                });
                localVideo.srcObject = stream;
                await network.addAudioStream(stream, SimpleP2P.Direction.SendOnly);
                await network.addVideoStream(stream, SimpleP2P.Direction.SendOnly);
                addLog("Microphone and camera added to the local stream.", "success");
            } catch (error) {
                addLog(`Media access error: ${error.message}`, "error");
            }
        };
        network.onDisconnect = () => {
            updateStatus("Disconnected", "disconnected");
            connectButton.disabled = false;
            disconnectButton.disabled = true;
            sendMessageButton.disabled = true;
            window.clearInterval(uiRefreshInterval);
            uiRefreshInterval = null;
            if (localVideo.srcObject) {
                localVideo.srcObject.getTracks().forEach((track) => track.stop());
                localVideo.srcObject = null;
            }
            remoteVideos.replaceChildren();
            network = null;
            renderPeers();
            addLog("Network connection terminated.");
        };
        network.onPeerJoin = (_peerId, peerName) => {
            addLog(`Peer joined: ${peerName}`);
            renderPeers();
        };
        network.onPeerLeave = (peerId) => {
            const video = document.getElementById(`video-${peerId}`);
            if (video) {
                video.pause();
                video.srcObject = null;
                video.remove();
            }
            addLog("Peer disconnected from the room.");
            renderPeers();
        };
        network.onMessage = (peerId, data) => {
            const peerName = network.peers.get(peerId)?.name || "Unknown";
            addLog(data.type === "message" ? `${peerName}: ${data.text}` : `Data received from ${peerName}.`, "success");
        };
        network.onTrack = (peerId, remoteStream, track) => {
            if (track?.kind !== "video") return;
            addLog(`Receiving video from ${peerId}.`, "success");
            const video = ensureRemoteVideo(peerId);
            video.srcObject = remoteStream;
            video.play().catch((error) => addLog(`Video playback failed: ${error.message}`, "error"));
        };
        network.connect();
    });

    disconnectButton.addEventListener("click", () => network?.disconnect());
    sendMessageButton.addEventListener("click", () => {
        if (!network) return;
        const payload = { type: "message", text: "Hello P2P Network!" };
        network.broadcast(payload);
        addLog(`You: ${payload.text}`);
    });
    artButton.addEventListener("click", () => {
        if (!network) {
            addLog("Connect to a room before opening the visual study.", "error");
            document.getElementById("session").scrollIntoView({ behavior: "smooth" });
            return;
        }
        const parameters = new URLSearchParams({
            room: roomInput.value.trim(), name: `${nameInput.value.trim()}_art`, "shape-morph": "35",
            "movement-amplitude": "60", dispersion: "0", "frame-randomness": "0", "phase-spread": "200",
            "length-variety": "100", "width-variety": "100", density: "500", "motion-speed": "1",
            "bend-variety": "100", "depth-variety": "100", "direction-variety": "0", "camera-scale": "220",
        });
        window.open(`./tests/manta-ray/?${parameters.toString()}`, "_blank", "noopener,noreferrer");
    });
})();
