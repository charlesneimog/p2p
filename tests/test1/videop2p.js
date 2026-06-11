const roomInput = document.getElementById("room-name");
const nameInput = document.getElementById("user-name");
const connectBtn = document.getElementById("btn-connect");
const disconnectBtn = document.getElementById("btn-disconnect");
const fullscreenBtn = document.getElementById("btn-fullscreen");
const statusEl = document.getElementById("status");
const stageEl = document.querySelector(".stage");
const remoteVideo = document.getElementById("remote-video");
const emptyState = document.getElementById("empty-state");
const logEl = document.getElementById("log");
const randomId = Math.floor(Math.random() * 10000)
    .toString()
    .padStart(4, "0");

nameInput.value = `receiver_${randomId}`;

let network = null;
function addLog(message, type = "info") {
    const line = document.createElement("div");
    line.textContent = `[${new Date().toLocaleTimeString("en-US", { hour12: false })}] ${message}`;
    line.style.color = type === "error" ? "#ffb4ab" : type === "success" ? "#9ad4aa" : "#f0f0ea";
    logEl.prepend(line);
}

function setStatus(message, state = "idle") {
    statusEl.textContent = message;
    statusEl.className = `status ${state === "connected" ? "connected" : state === "error" ? "error" : ""}`;
}

function clearRemoteVideo() {
    remoteVideo.pause();
    remoteVideo.srcObject = null;
    emptyState.hidden = false;
}

connectBtn.onclick = () => {
    const room = roomInput.value.trim();
    const name = nameInput.value.trim();

    if (!room || !name) {
        setStatus("Room and user name are required", "error");
        return;
    }

    if (typeof SimpleP2P === "undefined") {
        setStatus("p2p.js did not load", "error");
        return;
    }

    class ReceiveOnlyP2P extends SimpleP2P {
        _createPeerConnection(peerId, peer) {
            const pc = super._createPeerConnection(peerId, peer);
            pc.addTransceiver("video", { direction: "recvonly" });
            return pc;
        }
    }

    network = new ReceiveOnlyP2P(room, name);

    network.onLog = (message) => addLog(message);
    network.onError = (error) => {
        const message = error?.message || String(error);
        setStatus(message, "error");
        addLog(message, "error");
    };
    network.onConnect = (myId) => {
        setStatus(`Connected as ${name}`, "connected");
        connectBtn.disabled = true;
        disconnectBtn.disabled = false;
        addLog(`Joined room ${room} as ${myId.substring(0, 8)}`, "success");
    };
    network.onDisconnect = () => {
        setStatus("Disconnected");
        connectBtn.disabled = false;
        disconnectBtn.disabled = true;
        clearRemoteVideo();
        network = null;
    };
    network.onPeerJoin = (_peerId, peerName) => {
        addLog(`Peer joined: ${peerName}`);
    };
    network.onPeerLeave = () => {
        addLog("Peer left");
        clearRemoteVideo();
    };
    network.onTrack = (peerId, stream, track) => {
        if (track?.kind !== "video") return;

        remoteVideo.srcObject = stream;
        emptyState.hidden = true;
        remoteVideo.play().catch((error) => {
            addLog(`Video play failed: ${error.message}`, "error");
        });
        addLog(`Receiving video from ${peerId}`, "success");
    };

    setStatus("Connecting...");
    network.connect();
};

disconnectBtn.onclick = () => {
    if (network) network.disconnect();
};

fullscreenBtn.onclick = () => {
    if (!document.fullscreenElement) {
        stageEl.requestFullscreen().catch((error) => {
            addLog(`Fullscreen failed: ${error.message}`, "error");
        });
        return;
    }

    document.exitFullscreen();
};

document.addEventListener("fullscreenchange", () => {
    fullscreenBtn.textContent = document.fullscreenElement ? "Exit fullscreen" : "Fullscreen";
    requestAnimationFrame(() => {
        if (typeof windowResized === "function") windowResized();
    });
});
