const pageMode = document.body.dataset.videoMode || "sendrecv";
const pageLabel = document.body.dataset.videoLabel || "Video Test";

const roomInput = document.getElementById("room-name");
const nameInput = document.getElementById("user-name");
const connectBtn = document.getElementById("btn-connect");
const disconnectBtn = document.getElementById("btn-disconnect");
const statusEl = document.getElementById("status");
const localPanel = document.getElementById("local-panel");
const remotePanel = document.getElementById("remote-panel");
const localState = document.getElementById("local-state");
const remoteState = document.getElementById("remote-state");
const localVideo = document.getElementById("local-video");
const remoteList = document.getElementById("remote-list");
const messageInput = document.getElementById("message-text");
const sendMessageBtn = document.getElementById("btn-send-message");
const logEl = document.getElementById("log");

const randomId = Math.floor(Math.random() * 10000)
    .toString()
    .padStart(4, "0");

nameInput.value = `${pageMode}_video_${randomId}`;
document.title = `P2P ${pageLabel}`;

let network = null;
let localStream = null;
const remotePeers = new Map();

const canSendVideo = pageMode === "send" || pageMode === "sendrecv";
const canReceiveVideo = pageMode === "receive" || pageMode === "sendrecv";
const videoDirection = pageMode === "send" ? "sendonly" : pageMode === "receive" ? "recvonly" : "sendrecv";

localPanel.hidden = !canSendVideo;
remotePanel.hidden = !canReceiveVideo;

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

function setMessageEnabled(enabled) {
    sendMessageBtn.disabled = !enabled;
    messageInput.disabled = !enabled;
}

async function getLocalVideo() {
    localState.textContent = "Requesting camera...";
    localStream = await navigator.mediaDevices.getUserMedia({
        audio: false,
        video: {
            width: { ideal: 1280 },
            height: { ideal: 720 },
            facingMode: "user",
        },
    });
    localVideo.srcObject = localStream;
    await localVideo.play();
    localState.textContent = "Camera streaming";
    return localStream;
}

function clearLocalVideo() {
    if (localStream) {
        localStream.getTracks().forEach((track) => track.stop());
        localStream = null;
    }
    localVideo.pause();
    localVideo.srcObject = null;
    localState.textContent = canSendVideo ? "Camera idle" : "Send video disabled";
}

function clearRemoteVideo() {
    for (const remote of remotePeers.values()) {
        remote.video.pause();
        remote.video.srcObject = null;
        remote.node.remove();
    }
    remotePeers.clear();
    remoteState.textContent = canReceiveVideo ? "Waiting for remote video..." : "Receive video disabled";
}

function removeRemoteVideo(peerId) {
    const remote = remotePeers.get(peerId);
    if (!remote) return;

    remote.video.pause();
    remote.video.srcObject = null;
    remote.node.remove();
    remotePeers.delete(peerId);

    if (remotePeers.size === 0) {
        remoteState.textContent = "Waiting for remote video...";
    }
}

function ensureRemoteVideo(peerId, peerName) {
    let remote = remotePeers.get(peerId);
    if (remote) return remote;

    const node = document.createElement("div");
    node.className = "remote-peer";

    const title = document.createElement("strong");
    title.textContent = peerName || peerId.substring(0, 8);

    const video = document.createElement("video");
    video.autoplay = true;
    video.controls = true;
    video.playsInline = true;

    node.append(title, video);
    remoteList.appendChild(node);

    remote = { video, node };
    remotePeers.set(peerId, remote);
    return remote;
}

function peerName(peerId) {
    return network?.peers.get(peerId)?.name || peerId.substring(0, 8);
}

async function prepareMedia() {
    if (canSendVideo) {
        await getLocalVideo();
        await network.addVideoStream(localStream, videoDirection);
        addLog("Local camera added to the peer connection.", "success");
        return;
    }

    await network.addVideoStream(null, videoDirection);
    addLog("Video receive transceiver enabled.", "success");
}

connectBtn.onclick = async () => {
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

    setStatus("Connecting...");
    connectBtn.disabled = true;
    network = new SimpleP2P(room, name);

    network.onLog = (message) => addLog(message);
    network.onError = (error) => {
        const message = error?.message || String(error);
        setStatus(message, "error");
        addLog(message, "error");
    };
    network.onConnect = async (myId) => {
        setStatus(`Connected as ${name}`, "connected");
        disconnectBtn.disabled = false;
        setMessageEnabled(true);
        addLog(`Joined room ${room} as ${myId.substring(0, 8)}`, "success");

        try {
            await prepareMedia();
        } catch (error) {
            const message = error?.message || String(error);
            setStatus(message, "error");
            addLog(`Video setup failed: ${message}`, "error");
        }
    };
    network.onDisconnect = () => {
        setStatus("Disconnected");
        connectBtn.disabled = false;
        disconnectBtn.disabled = true;
        setMessageEnabled(false);
        clearLocalVideo();
        clearRemoteVideo();
        addLog("Network connection terminated.", "error");
        network = null;
    };
    network.onPeerJoin = (_peerId, name) => {
        addLog(`Peer joined: ${name}`);
    };
    network.onPeerLeave = (peerId) => {
        removeRemoteVideo(peerId);
        addLog(`Peer left: ${peerId}`);
    };
    network.onTrack = (peerId, stream, track) => {
        if (track?.kind !== "video" || !canReceiveVideo) return;

        const remote = ensureRemoteVideo(peerId, peerName(peerId));
        remote.video.srcObject = stream;
        remoteState.textContent = `${remotePeers.size} remote ${remotePeers.size === 1 ? "source" : "sources"}`;
        remote.video.play().catch((error) => {
            addLog(`Remote video play failed: ${error.message}`, "error");
        });
        addLog(`Receiving video from ${peerName(peerId)}`, "success");
    };
    network.onMessage = (peerId, data) => {
        const name = peerName(peerId);
        if (data?.type === "message") {
            addLog(`${name}: ${data.text}`, "success");
        } else {
            addLog(`Data received from ${name}.`);
        }
    };

    network.connect();
};

disconnectBtn.onclick = () => {
    if (network) network.disconnect();
};

sendMessageBtn.onclick = () => {
    const text = messageInput.value.trim() || `Hello from ${nameInput.value.trim()}`;
    if (!network) return;

    network.broadcast({ type: "message", text, timestamp: Date.now() });
    addLog(`Me: ${text}`, "success");
    messageInput.value = "";
};

messageInput.addEventListener("keydown", (event) => {
    if (event.key === "Enter" && !sendMessageBtn.disabled) {
        sendMessageBtn.click();
    }
});

setMessageEnabled(false);
clearLocalVideo();
clearRemoteVideo();
