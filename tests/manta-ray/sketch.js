let speed = 0.02;

let baseBodyPivot;
let bodyPivot;
let wingSegmentCount = 20;
let segmentLengths = createWingLengths(315, 115, 95, wingSegmentCount);
let crankRadii = createWingValues(6, 2, wingSegmentCount);
let crankPhases = createWingValues(0.0, 1.4, wingSegmentCount);
let centerCrankRadius = 28;
let wingCount = 200;
let wingRootStrokeWidth = 28;
let wingTipStrokeWidth = 5;
let wings = [];
let cameraMixer;
let cameraMixShader;
let cameraMixWidth = 640;
let cameraMixHeight = 480;
let cameraReady = false;
let drawingCameraTexture = false;
let remoteVideos = new Map();
let maxMixedVideos = 4;
let p2pNetwork = null;
let p2pStatusEl;
let p2pConnectBtn;
let p2pDisconnectBtn;
let p2pRoomInput;
let p2pNameInput;

// ─────────────────────────────────────
function setup() {
    createCanvas(windowWidth, windowHeight, WEBGL);
    pixelDensity(1);
    textureMode(IMAGE);
    baseBodyPivot = createVector(0, -90, 0);
    bodyPivot = baseBodyPivot.copy();
    wings = createRandomWings(wingCount);
    setupCameraMixer();
    setupP2PCamera();
}

// ─────────────────────────────────────
function draw() {
    background(245);
    orbitControl();
    rotateX(-0.0);
    let theta = frameCount * speed;
    let cameraTexture = renderMixedCameraTexture();
    drawingCameraTexture = Boolean(cameraTexture);

    if (drawingCameraTexture) {
        noStroke();
        beginShape(TRIANGLES);
        texture(cameraTexture);
    }

    for (let wing of wings) {
        let wingTheta = theta + wing.phaseOffset;
        bodyPivot = movingBodyPivot(wingTheta, wing.origin);
        drawWing(bodyPivot, wing.side, wingTheta, wing);
    }

    if (drawingCameraTexture) {
        endShape();
    }

    drawingCameraTexture = false;
}

// ─────────────────────────────────────
function setupCamera() {
    setupP2PCamera();
}

// ─────────────────────────────────────
function setupCameraMixer() {
    cameraMixer = createGraphics(cameraMixWidth, cameraMixHeight, WEBGL);
    cameraMixer.pixelDensity(1);
    cameraMixer.noStroke();
    cameraMixShader = cameraMixer.createShader(cameraMixVertexShader(), cameraMixFragmentShader());
}

// ─────────────────────────────────────
function setupP2PCamera() {
    p2pStatusEl = document.getElementById("p2p-status");
    p2pConnectBtn = document.getElementById("p2p-connect");
    p2pDisconnectBtn = document.getElementById("p2p-disconnect");
    p2pRoomInput = document.getElementById("p2p-room");
    p2pNameInput = document.getElementById("p2p-name");

    let params = new URLSearchParams(window.location.search);
    let randomId = Math.floor(Math.random() * 10000)
        .toString()
        .padStart(4, "0");

    p2pRoomInput.value = params.get("room") || "node-alpha";
    p2pNameInput.value = params.get("name") || `manta_ray_${randomId}`;
    p2pConnectBtn.onclick = connectP2PCamera;
    p2pDisconnectBtn.onclick = disconnectP2PCamera;

    if (params.get("autoconnect") !== "false") {
        connectP2PCamera();
    }
}

// ─────────────────────────────────────
function isCameraTextureReady() {
    return cameraReady;
}

// ─────────────────────────────────────
async function connectP2PCamera() {
    let room = p2pRoomInput.value.trim();
    let name = p2pNameInput.value.trim();

    if (!room || !name) {
        setP2PStatus("Room and name are required", true);
        return;
    }

    if (typeof SimpleP2P === "undefined") {
        setP2PStatus("p2p.js did not load", true);
        return;
    }

    disconnectP2PCamera();
    cameraReady = false;
    p2pConnectBtn.disabled = true;
    p2pRoomInput.disabled = true;
    p2pNameInput.disabled = true;
    setP2PStatus("Connecting...");

    p2pNetwork = new SimpleP2P(room, name);
    let network = p2pNetwork;
    network.onConnect = async () => {
        if (p2pNetwork !== network) return;

        p2pDisconnectBtn.disabled = false;
        setP2PStatus("Waiting for P2P cameras...");
        await network.addVideoStream(null, SimpleP2P.Direction.RecvOnly);
    };
    network.onDisconnect = () => {
        if (p2pNetwork && p2pNetwork !== network) return;

        clearP2PCamera();
        p2pNetwork = null;
        p2pConnectBtn.disabled = false;
        p2pDisconnectBtn.disabled = true;
        p2pRoomInput.disabled = false;
        p2pNameInput.disabled = false;
        setP2PStatus("Disconnected");
    };
    network.onPeerJoin = (_peerId, peerName) => {
        if (p2pNetwork !== network) return;

        setP2PStatus(`Peer joined: ${peerName}`);
    };
    network.onPeerLeave = (peerId) => {
        if (p2pNetwork !== network) return;

        removeRemoteVideo(peerId);
        updateCameraStatus();
    };
    network.onTrack = (peerId, stream, track) => {
        if (p2pNetwork !== network) return;
        if (track?.kind !== "video") return;

        let remote = ensureRemoteVideo(peerId);
        remote.stream = stream;
        remote.video.elt.srcObject = stream;
        remote.video.elt
            .play()
            .then(() => {
                remote.ready = true;
                updateCameraStatus();
            })
            .catch((error) => {
                setP2PStatus(`Video play failed: ${error.message}`, true);
            });
    };
    network.onError = (error) => {
        if (p2pNetwork !== network) return;

        setP2PStatus(error?.message || String(error), true);
    };

    network.connect();
}

// ─────────────────────────────────────
function disconnectP2PCamera() {
    if (p2pNetwork) {
        let network = p2pNetwork;
        p2pNetwork = null;
        network.disconnect();
        return;
    }

    clearP2PCamera();
}

// ─────────────────────────────────────
function clearP2PCamera() {
    cameraReady = false;

    for (let peerId of Array.from(remoteVideos.keys())) {
        removeRemoteVideo(peerId);
    }
}

// ─────────────────────────────────────
function setP2PStatus(message, isError = false) {
    if (!p2pStatusEl) return;

    p2pStatusEl.textContent = message;
    p2pStatusEl.style.color = isError ? "#ffb4ab" : "rgba(246, 248, 248, 0.8)";
}

// ─────────────────────────────────────
function peerLabel(peerId) {
    let peer = p2pNetwork?.peers.get(peerId);
    return peer?.name || peerId.substring(0, 8);
}

// ─────────────────────────────────────
function ensureRemoteVideo(peerId) {
    let remote = remoteVideos.get(peerId);
    if (remote) return remote;

    let video = createVideo([]);
    video.hide();
    video.elt.autoplay = true;
    video.elt.muted = true;
    video.elt.playsInline = true;

    remote = { video, stream: null, ready: false };
    remoteVideos.set(peerId, remote);
    return remote;
}

// ─────────────────────────────────────
function removeRemoteVideo(peerId) {
    let remote = remoteVideos.get(peerId);
    if (!remote) return;

    remote.video.elt.pause();
    remote.video.elt.srcObject = null;
    remote.video.remove();
    remoteVideos.delete(peerId);
}

// ─────────────────────────────────────
function readyRemoteVideos() {
    return Array.from(remoteVideos.values()).filter((remote) => {
        let video = remote.video.elt;
        return (
            remote.ready &&
            video.readyState >= HTMLMediaElement.HAVE_CURRENT_DATA &&
            video.videoWidth > 0 &&
            video.videoHeight > 0
        );
    });
}

// ─────────────────────────────────────
function renderMixedCameraTexture() {
    let readyVideos = readyRemoteVideos();
    let mixedVideos = readyVideos.slice(0, maxMixedVideos);
    cameraReady = mixedVideos.length > 0;

    if (!cameraReady) return null;

    cameraMixer.shader(cameraMixShader);
    cameraMixShader.setUniform("uVideoCount", mixedVideos.length);

    for (let i = 0; i < maxMixedVideos; i++) {
        let video = mixedVideos[min(i, mixedVideos.length - 1)].video;
        cameraMixShader.setUniform(`uVideo${i}`, video);
    }

    cameraMixer.rect(0, 0, cameraMixWidth, cameraMixHeight);
    return cameraMixer;
}

// ─────────────────────────────────────
function updateCameraStatus() {
    let readyCount = readyRemoteVideos().length;
    let totalCount = remoteVideos.size;

    if (readyCount === 0) {
        setP2PStatus(totalCount > 0 ? "Waiting for P2P video frames..." : "Waiting for P2P cameras...");
        return;
    }

    let mixedCount = min(readyCount, maxMixedVideos);
    let suffix = readyCount > maxMixedVideos ? ` (${mixedCount}/${readyCount} mixed)` : "";
    setP2PStatus(`GPU mixing ${mixedCount} P2P ${mixedCount === 1 ? "camera" : "cameras"}${suffix}`);
}

// ─────────────────────────────────────
function cameraMixVertexShader() {
    return `
precision mediump float;

attribute vec3 aPosition;
attribute vec2 aTexCoord;

varying vec2 vTexCoord;

void main() {
    vTexCoord = aTexCoord;
    vec4 position = vec4(aPosition, 1.0);
    position.xy = position.xy * 2.0 - 1.0;
    gl_Position = position;
}
`;
}

// ─────────────────────────────────────
function cameraMixFragmentShader() {
    return `
precision mediump float;

uniform sampler2D uVideo0;
uniform sampler2D uVideo1;
uniform sampler2D uVideo2;
uniform sampler2D uVideo3;
uniform int uVideoCount;

varying vec2 vTexCoord;

void main() {
    vec2 uv = vec2(vTexCoord.x, 1.0 - vTexCoord.y);
    vec4 color = texture2D(uVideo0, uv);
    float count = 1.0;

    if (uVideoCount > 1) {
        color += texture2D(uVideo1, uv);
        count += 1.0;
    }
    if (uVideoCount > 2) {
        color += texture2D(uVideo2, uv);
        count += 1.0;
    }
    if (uVideoCount > 3) {
        color += texture2D(uVideo3, uv);
        count += 1.0;
    }

    gl_FragColor = vec4((color / count).rgb, 1.0);
}
`;
}

// ─────────────────────────────────────
function movingBodyPivot(theta, origin = baseBodyPivot) {
    let centerY = sin(theta) * centerCrankRadius;
    return createVector(origin.x, origin.y + centerY, origin.z);
}

// ─────────────────────────────────────
function createRandomWings(count) {
    let randomWings = [];
    let marginX = 340;
    let marginY = 160;
    let rangeX = max(40, width / 2 - marginX);
    let rangeY = max(40, height / 2 - marginY);
    let rangeZ = 420;

    for (let i = 0; i < count; i++) {
        randomWings.push({
            origin: createVector(
                random(-rangeX, rangeX),
                random(-rangeY, rangeY),
                random(-rangeZ, rangeZ),
            ),
            side: random([1, -1]),
            phaseOffset: random(TWO_PI),
            textureAnchor: createVector(random(), random()),
            textureRotation: random(TWO_PI),
            textureScale: random(0.45, 1.35),
        });
    }

    return randomWings;
}

// ─────────────────────────────────────
function windowResized() {
    resizeCanvas(windowWidth, windowHeight);
    wings = createRandomWings(wingCount);
}

// ─────────────────────────────────────
function drawWing(pivot, side, theta, wing) {
    let baseAngle = side === 1 ? 0 : PI;
    let yToAngle = 0.012;

    let angles = [];
    let currentAngle = baseAngle;
    for (let i = 0; i < wingSegmentCount; i++) {
        let crankY = sin(theta + crankPhases[i]) * crankRadii[i];
        currentAngle += crankY * yToAngle;
        angles.push(currentAngle);
    }

    let joints = forwardKinematics(pivot, angles);
    drawSegments(joints, wing);
}

// ─────────────────────────────────────
function cameraUV(position, wing) {
    let localX = position.x - wing.origin.x;
    let localY = position.y - wing.origin.y;
    let textureCos = cos(wing.textureRotation);
    let textureSin = sin(wing.textureRotation);
    let rotatedX = localX * textureCos - localY * textureSin;
    let rotatedY = localX * textureSin + localY * textureCos;

    return {
        u: wrapCoordinate(wing.textureAnchor.x * cameraMixWidth + rotatedX * wing.textureScale, cameraMixWidth),
        v: wrapCoordinate(wing.textureAnchor.y * cameraMixHeight + rotatedY * wing.textureScale, cameraMixHeight),
    };
}

// ─────────────────────────────────────
function wrapCoordinate(value, size) {
    return ((value % size) + size) % size;
}

// ─────────────────────────────────────
function createWingValues(start, end, count) {
    let values = [];
    for (let i = 0; i < count; i++) {
        let t = count === 1 ? 0 : i / (count - 1);
        values.push(start + (end - start) * t);
    }
    return values;
}

// ─────────────────────────────────────
function createWingLengths(totalLength, startWeight, endWeight, count) {
    let weights = createWingValues(startWeight, endWeight, count);
    let weightTotal = weights.reduce((sum, weight) => sum + weight, 0);
    return weights.map((weight) => (weight / weightTotal) * totalLength);
}

// ─────────────────────────────────────
function forwardKinematics(start, angles) {
    let joints = [start.copy()];
    for (let i = 0; i < segmentLengths.length; i++) {
        let previous = joints[i];
        let next = createVector(
            previous.x + cos(angles[i]) * segmentLengths[i],
            previous.y + sin(angles[i]) * segmentLengths[i],
            previous.z,
        );
        joints.push(next);
    }
    return joints;
}

// ─────────────────────────────────────
function drawSegments(joints, wing) {
    if (!drawingCameraTexture) {
        stroke(20);
        strokeCap(ROUND);
        for (let i = 0; i < joints.length - 1; i++) {
            let t = i / (joints.length - 1);
            strokeWeight(getRibbonWidth(t));
            line(joints[i].x, joints[i].y, joints[i].z, joints[i + 1].x, joints[i + 1].y, joints[i + 1].z);
        }
        return;
    }

    addTexturedWingRibbon(joints, wing);
}

// ─────────────────────────────────────
function addTexturedWingRibbon(joints, wing) {
    let edges = createRibbonEdges(joints);

    for (let i = 0; i < edges.length - 1; i++) {
        addTexturedVertex(edges[i].left, wing);
        addTexturedVertex(edges[i + 1].left, wing);
        addTexturedVertex(edges[i + 1].right, wing);

        addTexturedVertex(edges[i].left, wing);
        addTexturedVertex(edges[i + 1].right, wing);
        addTexturedVertex(edges[i].right, wing);
    }
}

// ─────────────────────────────────────
function createRibbonEdges(joints) {
    let edges = [];

    for (let i = 0; i < joints.length; i++) {
        let previous = joints[max(0, i - 1)];
        let next = joints[min(joints.length - 1, i + 1)];
        let dx = next.x - previous.x;
        let dy = next.y - previous.y;
        let length = sqrt(dx * dx + dy * dy);

        if (length === 0) {
            edges.push({
                left: joints[i].copy(),
                right: joints[i].copy(),
            });
            continue;
        }

        let t = joints.length === 1 ? 0 : i / (joints.length - 1);
        let width = getRibbonWidth(t);
        let normalX = (-dy / length) * width / 2;
        let normalY = (dx / length) * width / 2;

        edges.push({
            left: createVector(joints[i].x + normalX, joints[i].y + normalY, joints[i].z),
            right: createVector(joints[i].x - normalX, joints[i].y - normalY, joints[i].z),
        });
    }

    return edges;
}

// ─────────────────────────────────────
function getRibbonWidth(t) {
    let rootFade = smoothstep(0.0, 0.12, t);
    let tipFade = 1 - smoothstep(0.74, 1.0, t);
    let bodyWidth = lerp(wingRootStrokeWidth, wingTipStrokeWidth, t);
    return max(1, bodyWidth * rootFade * tipFade);
}

// ─────────────────────────────────────
function smoothstep(edge0, edge1, value) {
    let t = constrain((value - edge0) / (edge1 - edge0), 0, 1);
    return t * t * (3 - 2 * t);
}

// ─────────────────────────────────────
function addTexturedVertex(point, wing) {
    let uv = cameraUV(point, wing);
    vertex(point.x, point.y, point.z, uv.u, uv.v);
}
