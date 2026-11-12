const ui = window.performanceUI;
const countdown = document.querySelector("#countdown");
const countdownValue = document.querySelector("#countdown-value");
const countdownLabel = document.querySelector("#countdown-label");

let client = null;
let selectedUsername = null;
let selectedRoom = null;
let signalingReady = false;
let connectionTimer = null;
let countdownTimer = null;
const remoteAudio = new Map();

// ─────────────────────────────────────
function stopRemoteAudio(peerId) {
    const audio = remoteAudio.get(peerId);
    if (!audio) return;

    audio.pause();
    audio.srcObject = null;
    audio.remove();
    remoteAudio.delete(peerId);
}

// ─────────────────────────────────────
function stopAllRemoteAudio() {
    [...remoteAudio.keys()].forEach(stopRemoteAudio);
}

// ─────────────────────────────────────
function stopCountdown() {
    if (countdownTimer !== null) {
        clearTimeout(countdownTimer);
        countdownTimer = null;
    }
    countdown.hidden = true;
}

// ─────────────────────────────────────
function isMainConnected(p2pClient = client) {
    if (!p2pClient) return false;

    return [...p2pClient.peers.values()].some(
        (peer) => peer.name === "main" && peer.pc?.connectionState === "connected" && peer.dc?.readyState === "open",
    );
}

// ─────────────────────────────────────
function updateConnectionStatus(p2pClient = client) {
    if (!p2pClient || p2pClient !== client || !signalingReady) return;

    if (isMainConnected(p2pClient)) {
        ui.setConnectionState("connected", `Conectado · ${selectedUsername} · Pd pronto`);
    } else {
        ui.setConnectionState("waiting", `Na sala ${selectedRoom} · aguardando Pd (main)`);
    }
}

// ─────────────────────────────────────
function disconnect() {
    // Invalidate callbacks before SimpleP2P.disconnect() fires onDisconnect.
    const oldClient = client;
    client = null;
    signalingReady = false;
    selectedUsername = null;
    selectedRoom = null;

    if (connectionTimer !== null) {
        clearTimeout(connectionTimer);
        connectionTimer = null;
    }

    stopCountdown();
    stopAllRemoteAudio();

    if (oldClient) oldClient.disconnect();
    ui.stopMicrophone();
    ui.setConnectionState("disconnected");
}

// ─────────────────────────────────────
function connectionFailed(message) {
    disconnect();
    ui.setConnectionState("error", message);
}

// ─────────────────────────────────────
function startCountdown() {
    if (!client || !isMainConnected(client) || countdownTimer !== null) return;

    const sendingClient = client;
    let seconds = 3;

    countdownLabel.textContent = "A performance começa em";
    countdownValue.hidden = false;
    countdownValue.textContent = String(seconds);
    countdown.hidden = false;

    function tick() {
        if (client !== sendingClient || !isMainConnected(sendingClient)) {
            stopCountdown();
            updateConnectionStatus();
            return;
        }

        seconds -= 1;
        if (seconds > 0) {
            countdownValue.textContent = String(seconds);
            countdownTimer = setTimeout(tick, 1000);
            return;
        }

        countdownTimer = null;
        sendingClient.broadcast("start");
        countdown.hidden = true;
    }

    countdownTimer = setTimeout(tick, 1000);
}

// ─────────────────────────────────────
function newEvent(scoreEvent) {}

// ─────────────────────────────────────
async function newScore(scorePath) {
    if (typeof scorePath !== "string" || !scorePath || scorePath.startsWith("/") || scorePath.includes("..")) {
        throw new Error("Invalid score path");
    }

    const response = await fetch("scores/" + scorePath);
    if (!response.ok) throw new Error(`Could not load score (${response.status})`);
    document.getElementById("score").innerHTML = await response.text();
}

// ─────────────────────────────────────
function handleMessageForMe(parameters, peerId) {
    const [method, ...parsedParameters] = parameters;

    if (method === "newscore") {
        void newScore(parsedParameters[0]).catch((error) => console.error("Score loading failed:", error));
    } else if (method === "another") {
        // Reserved for other Pure Data messages.
    }
}

// ─────────────────────────────────────
function routeMessage(username, peerId, message) {
    let parts;

    if (Array.isArray(message)) {
        parts = message;
    } else if (message?.type === "message" && typeof message.text === "string") {
        parts = message.text.trim().split(/\s+/);
    } else if (typeof message === "string") {
        parts = message.trim().split(/\s+/);
    } else {
        return;
    }

    const [recipient, ...parameters] = parts;
    if (recipient !== username) return;
    handleMessageForMe(parameters, peerId);
}

// ─────────────────────────────────────
function playMainAudio(p2pClient, peerId, track) {
    const peer = p2pClient.peers.get(peerId);
    if (peer?.name !== "main" || track.kind !== "audio") return;

    stopRemoteAudio(peerId);

    const incomingStream = new MediaStream([track]);
    const audio = document.createElement("audio");
    audio.hidden = true;
    audio.autoplay = true;
    audio.playsInline = true;
    audio.srcObject = incomingStream;
    document.body.append(audio);
    remoteAudio.set(peerId, audio);

    track.addEventListener("ended", () => stopRemoteAudio(peerId), { once: true });
    audio.play().catch((error) => {
        console.warn("Incoming audio playback was blocked by the browser.", error);
    });
}

// ─────────────────────────────────────
async function joinRoom({ room, username }) {
    if (!room || !username || /\s/u.test(room) || /\s/u.test(username)) {
        throw new Error("Sala e nome de usuário devem ser preenchidos, sem espaços.");
    }
    if (typeof SimpleP2P !== "function") {
        throw new Error("p2p.js não foi carregado.");
    }

    disconnect();
    selectedRoom = room;
    selectedUsername = username;
    ui.setConnectionState("connecting", "Ativando microfone…");

    try {
        // Use the microphone stream processed by the gain control in index.html.
        const audioStream = await ui.startMicrophone();
        if (selectedUsername !== username || selectedRoom !== room) return;

        // p2p.js builds a URL with ?room=... without escaping the room value.
        const nextClient = new SimpleP2P(encodeURIComponent(room), username, {
            audio: {
                codec: "audio/opus",
                maxBitrate: 510000,
            },
        });
        client = nextClient;

        nextClient.onConnect = () => {
            if (client !== nextClient) return;
            signalingReady = true;
            if (connectionTimer !== null) {
                clearTimeout(connectionTimer);
                connectionTimer = null;
            }
            updateConnectionStatus(nextClient);
        };

        nextClient.onDisconnect = () => {
            if (client !== nextClient) return;
            disconnect();
            ui.setConnectionState("disconnected", "Conexão com o servidor encerrada");
        };

        nextClient.onError = (error) => {
            if (client !== nextClient) return;
            console.error(error);
            connectionFailed("Erro de conexão. Tente novamente.");
        };

        nextClient.onTrack = (peerId, _stream, track) => {
            if (client !== nextClient) return;
            playMainAudio(nextClient, peerId, track);
        };

        nextClient.onMessage = (peerId, message) => {
            if (client !== nextClient) return;
            routeMessage(username, peerId, message);
        };

        nextClient.onPeerStateChange = () => {
            if (client !== nextClient) return;
            if (countdownTimer !== null && !isMainConnected(nextClient)) stopCountdown();
            updateConnectionStatus(nextClient);
        };

        nextClient.onPeerLeave = (peerId) => {
            stopRemoteAudio(peerId);
            if (client !== nextClient) return;
            if (countdownTimer !== null && !isMainConnected(nextClient)) stopCountdown();
            updateConnectionStatus(nextClient);
        };

        nextClient.onLog = (message) => console.info(`[p2p] ${message}`);

        await nextClient.addAudioStream(audioStream, SimpleP2P.Direction.SendRecv);
        if (client !== nextClient) return;
        nextClient.connect();
        ui.setConnectionState("connecting", `Conectando à sala ${room}…`);

        connectionTimer = setTimeout(() => {
            if (client === nextClient && !signalingReady) {
                connectionFailed("Sem resposta do servidor de sinalização.");
            }
        }, 20000);
    } catch (error) {
        console.error(error);
        connectionFailed(error?.message || "Não foi possível conectar.");
    }
}

// ─────────────────────────────────────
async function updateMicrophoneStream(stream) {
    const activeClient = client;
    if (!activeClient) return;

    await activeClient.addAudioStream(stream, stream ? SimpleP2P.Direction.SendRecv : SimpleP2P.Direction.RecvOnly);
}

// ─────────────────────────────────────
ui.setHandlers({
    onConnect: joinRoom,
    onDisconnect: disconnect,
    onMicrophoneChange: updateMicrophoneStream,
});

window.addEventListener("beforeunload", disconnect);
