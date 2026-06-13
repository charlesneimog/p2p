const pageMode = document.body.dataset.audioMode || "sendrecv";
const pageLabel = document.body.dataset.audioLabel || "Audio Test";

const roomInput = document.getElementById("room-name");
const nameInput = document.getElementById("user-name");
const connectBtn = document.getElementById("btn-connect");
const disconnectBtn = document.getElementById("btn-disconnect");
const statusEl = document.getElementById("status");
const localPanel = document.getElementById("local-panel");
const remotePanel = document.getElementById("remote-panel");
const localState = document.getElementById("local-state");
const remoteState = document.getElementById("remote-state");
const localMeter = document.getElementById("local-meter");
const remoteList = document.getElementById("remote-list");
const messageInput = document.getElementById("message-text");
const sendMessageBtn = document.getElementById("btn-send-message");
const logEl = document.getElementById("log");

const randomId = Math.floor(Math.random() * 10000)
    .toString()
    .padStart(4, "0");

nameInput.value = `${pageMode}_${randomId}`;
document.title = `P2P ${pageLabel}`;

let network = null;
let localStream = null;
let audioContext = null;
let localMeterFrame = null;
const remotePeers = new Map();

const canSendAudio = pageMode === "send" || pageMode === "sendrecv";
const canReceiveAudio = pageMode === "receive" || pageMode === "sendrecv";
const audioDirection = pageMode === "send" ? "sendonly" : pageMode === "receive" ? "recvonly" : "sendrecv";

localPanel.hidden = !canSendAudio;
remotePanel.hidden = !canReceiveAudio;

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

function ensureAudioContext() {
    if (!audioContext) {
        const AudioContextClass = window.AudioContext || window.webkitAudioContext;
        audioContext = new AudioContextClass();
    }
    if (audioContext.state === "suspended") {
        audioContext.resume();
    }
    return audioContext;
}

function watchMeter(stream, meterEl) {
    const context = ensureAudioContext();
    const source = context.createMediaStreamSource(stream);
    const analyser = context.createAnalyser();
    let frame = null;

    analyser.fftSize = 1024;
    const samples = new Uint8Array(analyser.fftSize);
    source.connect(analyser);

    function draw() {
        analyser.getByteTimeDomainData(samples);
        let total = 0;
        for (const sample of samples) {
            const centered = sample - 128;
            total += centered * centered;
        }
        const rms = Math.sqrt(total / samples.length) / 128;
        meterEl.style.width = `${Math.min(100, Math.max(2, rms * 180))}%`;
        frame = requestAnimationFrame(draw);
    }

    draw();

    return () => {
        if (frame !== null) cancelAnimationFrame(frame);
        source.disconnect();
        meterEl.style.width = "0%";
    };
}

async function getLocalAudio() {
    localState.textContent = "Requesting microphone...";
    localStream = await navigator.mediaDevices.getUserMedia({
        audio: {
            echoCancellation: false,
            noiseSuppression: false,
            autoGainControl: false,
            channelCount: 1,
        },
        video: false,
    });
    localMeterFrame = watchMeter(localStream, localMeter);
    localState.textContent = "Microphone streaming";
    return localStream;
}

function clearLocalAudio() {
    if (localMeterFrame) {
        localMeterFrame();
        localMeterFrame = null;
    }
    if (localStream) {
        localStream.getTracks().forEach((track) => track.stop());
        localStream = null;
    }
    localState.textContent = canSendAudio ? "Microphone idle" : "Send audio disabled";
}

function clearRemoteAudio() {
    for (const remote of remotePeers.values()) {
        remote.stopMeter?.();
        remote.audio.pause();
        remote.audio.srcObject = null;
        remote.node.remove();
    }
    remotePeers.clear();
    remoteState.textContent = canReceiveAudio ? "Waiting for remote audio..." : "Receive audio disabled";
}

function removeRemoteAudio(peerId) {
    const remote = remotePeers.get(peerId);
    if (!remote) return;

    remote.stopMeter?.();
    remote.audio.pause();
    remote.audio.srcObject = null;
    remote.node.remove();
    remotePeers.delete(peerId);

    if (remotePeers.size === 0) {
        remoteState.textContent = "Waiting for remote audio...";
    }
}

function ensureRemoteAudio(peerId, peerName) {
    let remote = remotePeers.get(peerId);
    if (remote) return remote;

    const node = document.createElement("div");
    node.className = "remote-peer";

    const title = document.createElement("strong");
    title.textContent = peerName || peerId.substring(0, 8);

    const audio = document.createElement("audio");
    audio.autoplay = true;
    audio.controls = true;
    audio.playsInline = true;

    const meter = document.createElement("div");
    meter.className = "meter";
    const meterFill = document.createElement("span");
    meter.appendChild(meterFill);

    node.append(title, audio, meter);
    remoteList.appendChild(node);

    remote = { audio, meterFill, node, stopMeter: null };
    remotePeers.set(peerId, remote);
    return remote;
}

function peerName(peerId) {
    return network?.peers.get(peerId)?.name || peerId.substring(0, 8);
}

async function prepareMedia() {
    if (canSendAudio) {
        await getLocalAudio();
        await network.addAudioStream(localStream, audioDirection);
        addLog("Local microphone added to the peer connection.", "success");
        return;
    }

    await network.addAudioStream(null, audioDirection);
    addLog("Audio receive transceiver enabled.", "success");
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
            addLog(`Audio setup failed: ${message}`, "error");
        }
    };
    network.onDisconnect = () => {
        setStatus("Disconnected");
        connectBtn.disabled = false;
        disconnectBtn.disabled = true;
        setMessageEnabled(false);
        clearLocalAudio();
        clearRemoteAudio();
        addLog("Network connection terminated.", "error");
        network = null;
    };
    network.onPeerJoin = (_peerId, name) => {
        addLog(`Peer joined: ${name}`);
    };
    network.onPeerLeave = (peerId) => {
        removeRemoteAudio(peerId);
        addLog(`Peer left: ${peerId}`);
    };
    network.onTrack = (peerId, stream, track) => {
        if (track?.kind !== "audio" || !canReceiveAudio) return;

        const remote = ensureRemoteAudio(peerId, peerName(peerId));
        remote.audio.srcObject = stream;
        remote.stopMeter?.();
        remote.stopMeter = watchMeter(stream, remote.meterFill);
        remoteState.textContent = `${remotePeers.size} remote ${remotePeers.size === 1 ? "source" : "sources"}`;
        remote.audio.play().catch((error) => {
            addLog(`Remote audio play failed: ${error.message}`, "error");
        });
        addLog(`Receiving audio from ${peerName(peerId)}`, "success");
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
clearLocalAudio();
clearRemoteAudio();
