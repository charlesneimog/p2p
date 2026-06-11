const roomInput = document.getElementById("room-name");
const nameInput = document.getElementById("user-name");
const connectBtn = document.getElementById("btn-connect");
const disconnectBtn = document.getElementById("btn-disconnect");
const fullscreenBtn = document.getElementById("btn-fullscreen");
const statusEl = document.getElementById("status");
const stageEl = document.querySelector(".stage");
const remoteMix = document.getElementById("remote-mix");
const emptyState = document.getElementById("empty-state");
const mixCount = document.getElementById("mix-count");
const logEl = document.getElementById("log");
const randomId = Math.floor(Math.random() * 10000)
    .toString()
    .padStart(4, "0");

nameInput.value = `receiver_${randomId}`;

let network = null;
const remoteVideos = new Map();
const mixContext = remoteMix.getContext("2d", { alpha: false });
let mixAnimationId = null;

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
    stopMixLoop();
    for (const video of remoteVideos.values()) {
        video.pause();
        video.srcObject = null;
        video.remove();
    }
    remoteVideos.clear();
    mixContext.clearRect(0, 0, remoteMix.width, remoteMix.height);
    emptyState.hidden = false;
    updateMixCount(0);
}

function removeRemoteVideo(peerId) {
    const video = remoteVideos.get(peerId);
    if (!video) return;

    video.pause();
    video.srcObject = null;
    video.remove();
    remoteVideos.delete(peerId);
    emptyState.hidden = remoteVideos.size > 0;

    if (remoteVideos.size === 0) {
        stopMixLoop();
        mixContext.clearRect(0, 0, remoteMix.width, remoteMix.height);
        updateMixCount(0);
    }
}

function ensureRemoteVideo(peerId) {
    let video = remoteVideos.get(peerId);
    if (video) return video;

    video = document.createElement("video");
    video.autoplay = true;
    video.muted = true;
    video.playsInline = true;
    video.dataset.peerId = peerId;
    video.style.display = "none";
    stageEl.appendChild(video);
    remoteVideos.set(peerId, video);

    return video;
}

function startMixLoop() {
    if (mixAnimationId !== null) return;
    mixAnimationId = requestAnimationFrame(drawMixedVideos);
}

function stopMixLoop() {
    if (mixAnimationId === null) return;
    cancelAnimationFrame(mixAnimationId);
    mixAnimationId = null;
}

function resizeMixCanvas() {
    const rect = stageEl.getBoundingClientRect();
    const scale = Math.min(window.devicePixelRatio || 1, 2);
    const nextWidth = Math.max(1, Math.floor(rect.width * scale));
    const nextHeight = Math.max(1, Math.floor(rect.height * scale));

    if (remoteMix.width !== nextWidth || remoteMix.height !== nextHeight) {
        remoteMix.width = nextWidth;
        remoteMix.height = nextHeight;
    }
}

function drawVideoCover(video) {
    const canvasAspect = remoteMix.width / remoteMix.height;
    const videoAspect = video.videoWidth / video.videoHeight;
    let drawWidth = remoteMix.width;
    let drawHeight = remoteMix.height;
    let drawX = 0;
    let drawY = 0;

    if (canvasAspect > videoAspect) {
        drawHeight = remoteMix.width / videoAspect;
        drawY = (remoteMix.height - drawHeight) / 2;
    } else {
        drawWidth = remoteMix.height * videoAspect;
        drawX = (remoteMix.width - drawWidth) / 2;
    }

    mixContext.drawImage(video, drawX, drawY, drawWidth, drawHeight);
}

function drawMixedVideos() {
    mixAnimationId = null;
    resizeMixCanvas();

    const activeVideos = Array.from(remoteVideos.values()).filter(
        (video) => video.readyState >= HTMLMediaElement.HAVE_CURRENT_DATA && video.videoWidth && video.videoHeight,
    );

    mixContext.globalAlpha = 1;
    mixContext.globalCompositeOperation = "source-over";
    mixContext.fillStyle = "#000000";
    mixContext.fillRect(0, 0, remoteMix.width, remoteMix.height);

    mixContext.globalCompositeOperation = "lighter";
    mixContext.globalAlpha = activeVideos.length > 0 ? 1 / activeVideos.length : 1;
    activeVideos.forEach((video) => {
        drawVideoCover(video);
    });

    mixContext.globalAlpha = 1;
    mixContext.globalCompositeOperation = "source-over";
    emptyState.hidden = activeVideos.length > 0;
    updateMixCount(activeVideos.length);

    if (remoteVideos.size > 0) {
        mixAnimationId = requestAnimationFrame(drawMixedVideos);
    }
}

function updateMixCount(count) {
    mixCount.textContent = `${count} ${count === 1 ? "source" : "sources"}`;
}

window.remoteVideoMixer = {
    source: remoteMix,
    hasActiveVideos: () =>
        Array.from(remoteVideos.values()).some(
            (video) => video.readyState >= HTMLMediaElement.HAVE_CURRENT_DATA && video.videoWidth && video.videoHeight,
        ),
};

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
    network.onPeerLeave = (peerId) => {
        removeRemoteVideo(peerId);
        addLog(`Peer left: ${peerId}`);
    };
    network.onTrack = (peerId, stream, track) => {
        if (track?.kind !== "video") return;

        const remoteVideo = ensureRemoteVideo(peerId);
        remoteVideo.srcObject = stream;
        emptyState.hidden = true;
        remoteVideo.play().catch((error) => {
            addLog(`Video play failed: ${error.message}`, "error");
        });
        startMixLoop();
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
