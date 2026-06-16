let targetFrameRate = 20;
let speed = 0.03;
let sceneMotionSpeed = 0.012;
let sceneZoomAmount = 0.22;
let sceneMotionRange = {
    x: 180,
    y: 110,
    z: 260,
};

let wingSegmentCount = 20;
let segmentLengths = createWingLengths(315, 115, 95, wingSegmentCount);
let crankRadii = createWingValues(6, 2, wingSegmentCount);
let crankPhases = createWingValues(0.0, 1.4, wingSegmentCount);
let centerCrankRadius = 28;
let wingCount = 500;
let cylinderSideCount = 4;
let cylinderSegmentSubdivisions = 36;
let cylinderRenderer;
let wings = [];
let cameraMixer;
let cameraMixShader;
let cameraMixWidth = 640;
let cameraMixHeight = 480;
let cameraReady = false;
let remoteVideos = new Map();
let maxMixedVideos = 8;
let p2pNetwork = null;
let p2pStatusEl;
let p2pConnectBtn;
let p2pDisconnectBtn;
let p2pRoomInput;
let p2pNameInput;
let shapeMorphInput;
let movementAmplitudeInput;
let dispersionInput;
let frameRandomnessInput;
let phaseSpreadInput;
let lengthVarietyInput;
let widthVarietyInput;
let densityInput;
let motionSpeedInput;
let bendVarietyInput;
let depthVarietyInput;
let cameraScaleInput;
let startupSliderValues = {
    "shape-morph": 35,
    "movement-amplitude": 60,
    dispersion: 0,
    "frame-randomness": 0,
    "phase-spread": 200,
    "length-variety": 100,
    "width-variety": 100,
    density: 500,
    "motion-speed": 1,
    "bend-variety": 100,
    "depth-variety": 100,
    "camera-scale": 220,
};

// ─────────────────────────────────────
function setup() {
    let canvasSize = getCanvasSize();
    let canvas = createCanvas(canvasSize.width, canvasSize.height, WEBGL);
    canvas.parent("manta-canvas");
    pixelDensity(1);
    frameRate(targetFrameRate);
    textureMode(IMAGE);
    wings = createRandomWings(wingCount);
    setupCylinderRenderer();
    setupCameraMixer();
    setupP2PCamera();
}

// ─────────────────────────────────────
function draw() {
    background(7, 19, 31);
    orbitControl();
    rotateX(-0.0);
    let theta = frameCount * speed;
    let motionSpeed = getMotionSpeed();
    theta *= motionSpeed;
    applySceneMotion(frameCount * sceneMotionSpeed * motionSpeed);
    let cameraTexture = renderMixedCameraTexture();
    drawGpuCylinders(theta, cameraTexture);
}

// ─────────────────────────────────────
function applySceneMotion(theta) {
    let zoom = 1 + sin(theta * 0.72) * sceneZoomAmount;
    let x = sin(theta * 0.91) * sceneMotionRange.x;
    let y = sin(theta * 1.13 + 1.8) * sceneMotionRange.y;
    let z = sin(theta * 0.67 + 3.4) * sceneMotionRange.z;

    translate(x, y, z);
    scale(zoom);
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
    shapeMorphInput = document.getElementById("shape-morph");
    movementAmplitudeInput = document.getElementById("movement-amplitude");
    dispersionInput = document.getElementById("dispersion");
    frameRandomnessInput = document.getElementById("frame-randomness");
    phaseSpreadInput = document.getElementById("phase-spread");
    lengthVarietyInput = document.getElementById("length-variety");
    widthVarietyInput = document.getElementById("width-variety");
    densityInput = document.getElementById("density");
    motionSpeedInput = document.getElementById("motion-speed");
    bendVarietyInput = document.getElementById("bend-variety");
    depthVarietyInput = document.getElementById("depth-variety");
    cameraScaleInput = document.getElementById("camera-scale");

    let params = new URLSearchParams(window.location.search);
    let randomId = Math.floor(Math.random() * 10000)
        .toString()
        .padStart(4, "0");

    p2pRoomInput.value = params.get("room") || "node-alpha";
    p2pNameInput.value = params.get("name") || `manta_ray_${randomId}`;
    applyStartupSliderValues();
    p2pConnectBtn.onclick = connectP2PCamera;
    p2pDisconnectBtn.onclick = disconnectP2PCamera;

    if (params.get("autoconnect") !== "false") {
        connectP2PCamera();
    }
}

// ─────────────────────────────────────
function applyStartupSliderValues() {
    setSliderValue(shapeMorphInput, startupSliderValues["shape-morph"]);
    setSliderValue(movementAmplitudeInput, startupSliderValues["movement-amplitude"]);
    setSliderValue(dispersionInput, startupSliderValues.dispersion);
    setSliderValue(frameRandomnessInput, startupSliderValues["frame-randomness"]);
    setSliderValue(phaseSpreadInput, startupSliderValues["phase-spread"]);
    setSliderValue(lengthVarietyInput, startupSliderValues["length-variety"]);
    setSliderValue(widthVarietyInput, startupSliderValues["width-variety"]);
    setSliderValue(densityInput, startupSliderValues.density);
    setSliderValue(motionSpeedInput, startupSliderValues["motion-speed"]);
    setSliderValue(bendVarietyInput, startupSliderValues["bend-variety"]);
    setSliderValue(depthVarietyInput, startupSliderValues["depth-variety"]);
    setSliderValue(cameraScaleInput, startupSliderValues["camera-scale"]);
}

// ─────────────────────────────────────
function setSliderValue(input, value) {
    if (!input) return;

    input.value = constrain(value, Number(input.min), Number(input.max));
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
            lengthScale: random(0.58, 1.72),
            bendScale: random(0.35, 1.85),
            depthScale: random(0.35, 2.4),
            textureAnchor: createVector(random(), random()),
            textureRotation: random(TWO_PI),
            textureScale: random(0.45, 1.35),
            ...createCylinderWidthProfile(),
            cylinderColor: [random(30, 95) / 255, random(65, 130) / 255, random(105, 185) / 255],
        });
    }

    return randomWings;
}

// ─────────────────────────────────────
function windowResized() {
    let canvasSize = getCanvasSize();
    resizeCanvas(canvasSize.width, canvasSize.height);
    wings = createRandomWings(wingCount);
}

// ─────────────────────────────────────
function getCanvasSize() {
    let canvasContainer = document.getElementById("manta-canvas");

    if (!canvasContainer) {
        return {
            width: windowWidth,
            height: windowHeight,
        };
    }

    return {
        width: canvasContainer.clientWidth,
        height: canvasContainer.clientHeight,
    };
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
function setupCylinderRenderer() {
    cylinderRenderer = createCylinderRenderer();
}

// ─────────────────────────────────────
function createCylinderRenderer() {
    let gl = drawingContext;
    let program = createGlProgram(gl, cylinderVertexShader(), cylinderFragmentShader());
    let mesh = createCylinderParameterMesh();
    let vertexBuffer = gl.createBuffer();
    let indexBuffer = gl.createBuffer();
    let cameraFrameTexture = createCameraFrameTexture(gl);
    let uniforms = getCylinderUniforms(gl, program);

    gl.bindBuffer(gl.ARRAY_BUFFER, vertexBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, mesh.vertices, gl.STATIC_DRAW);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, indexBuffer);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, mesh.indices, gl.STATIC_DRAW);
    gl.useProgram(program);
    gl.uniform1fv(uniforms.segmentLengths, segmentLengths);
    gl.uniform1fv(uniforms.crankRadii, crankRadii);
    gl.uniform1fv(uniforms.crankPhases, crankPhases);

    return {
        gl,
        program,
        vertexBuffer,
        indexBuffer,
        cameraFrameTexture,
        indexCount: mesh.indices.length,
        attributes: {
            joint: gl.getAttribLocation(program, "aJoint"),
            sideAngle: gl.getAttribLocation(program, "aSideAngle"),
            radiusScale: gl.getAttribLocation(program, "aRadiusScale"),
        },
        uniforms,
    };
}

// ─────────────────────────────────────
function createCylinderParameterMesh() {
    let vertices = [];
    let indices = [];
    let ringCount = wingSegmentCount * cylinderSegmentSubdivisions + 1;
    let ringVertexCount = ringCount * cylinderSideCount;

    for (let ring = 0; ring < ringCount; ring++) {
        let joint = ring / cylinderSegmentSubdivisions;
        for (let side = 0; side < cylinderSideCount; side++) {
            let sideAngle = (side / cylinderSideCount) * TWO_PI;
            vertices.push(joint, sideAngle, 1);
        }
    }

    let startCenter = ringVertexCount;
    let endCenter = ringVertexCount + 1;
    vertices.push(0, 0, 0, wingSegmentCount, 0, 0);

    for (let ring = 0; ring < ringCount - 1; ring++) {
        for (let side = 0; side < cylinderSideCount; side++) {
            let sideNext = (side + 1) % cylinderSideCount;
            let current = ring * cylinderSideCount + side;
            let currentNext = ring * cylinderSideCount + sideNext;
            let next = (ring + 1) * cylinderSideCount + side;
            let nextSide = (ring + 1) * cylinderSideCount + sideNext;

            indices.push(current, next, nextSide, current, nextSide, currentNext);
        }
    }

    for (let side = 0; side < cylinderSideCount; side++) {
        let sideNext = (side + 1) % cylinderSideCount;
        let endBase = (ringCount - 1) * cylinderSideCount;

        indices.push(startCenter, sideNext, side);
        indices.push(endCenter, endBase + side, endBase + sideNext);
    }

    return {
        vertices: new Float32Array(vertices),
        indices: new Uint16Array(indices),
    };
}

// ─────────────────────────────────────
function getCylinderUniforms(gl, program) {
    return {
        projectionMatrix: gl.getUniformLocation(program, "uProjectionMatrix"),
        modelViewMatrix: gl.getUniformLocation(program, "uModelViewMatrix"),
        theta: gl.getUniformLocation(program, "uTheta"),
        side: gl.getUniformLocation(program, "uSide"),
        origin: gl.getUniformLocation(program, "uOrigin"),
        textureAnchor: gl.getUniformLocation(program, "uTextureAnchor"),
        textureRotation: gl.getUniformLocation(program, "uTextureRotation"),
        textureScale: gl.getUniformLocation(program, "uTextureScale"),
        cylinderColor: gl.getUniformLocation(program, "uCylinderColor"),
        shapeMorph: gl.getUniformLocation(program, "uShapeMorph"),
        movementAmplitude: gl.getUniformLocation(program, "uMovementAmplitude"),
        dispersion: gl.getUniformLocation(program, "uDispersion"),
        frameRandomness: gl.getUniformLocation(program, "uFrameRandomness"),
        lengthScale: gl.getUniformLocation(program, "uLengthScale"),
        bendScale: gl.getUniformLocation(program, "uBendScale"),
        depthScale: gl.getUniformLocation(program, "uDepthScale"),
        widthVariety: gl.getUniformLocation(program, "uWidthVariety"),
        cameraScale: gl.getUniformLocation(program, "uCameraScale"),
        useCameraTexture: gl.getUniformLocation(program, "uUseCameraTexture"),
        cameraTexture: gl.getUniformLocation(program, "uCameraTexture"),
        segmentLengths: gl.getUniformLocation(program, "uSegmentLengths"),
        crankRadii: gl.getUniformLocation(program, "uCrankRadii"),
        crankPhases: gl.getUniformLocation(program, "uCrankPhases"),
        cylinderWidthProfile: gl.getUniformLocation(program, "uCylinderWidthProfile"),
        cylinderWaveAmount: gl.getUniformLocation(program, "uCylinderWaveAmount"),
    };
}

// ─────────────────────────────────────
function drawGpuCylinders(theta, cameraTexture) {
    let renderer = cylinderRenderer;
    let gl = renderer.gl;
    let useCameraTexture = updateCameraFrameTexture(gl, renderer.cameraFrameTexture, cameraTexture);
    let shapeMorph = getShapeMorph();
    let movementAmplitude = getMovementAmplitude();
    let dispersion = getDispersion();
    let frameRandomness = getFrameRandomness();
    let phaseSpread = getPhaseSpread();
    let lengthVariety = getLengthVariety();
    let widthVariety = getWidthVariety();
    let bendVariety = getBendVariety();
    let depthVariety = getDepthVariety();
    let cameraScale = getCameraScale();
    let drawCount = getDensityCount();

    gl.useProgram(renderer.program);
    gl.bindBuffer(gl.ARRAY_BUFFER, renderer.vertexBuffer);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, renderer.indexBuffer);
    gl.enable(gl.DEPTH_TEST);

    setCylinderAttribute(gl, renderer.attributes.joint, 3, 0);
    setCylinderAttribute(gl, renderer.attributes.sideAngle, 3, 1);
    setCylinderAttribute(gl, renderer.attributes.radiusScale, 3, 2);

    gl.uniformMatrix4fv(renderer.uniforms.projectionMatrix, false, _renderer.uPMatrix.mat4);
    gl.uniformMatrix4fv(renderer.uniforms.modelViewMatrix, false, _renderer.uMVMatrix.mat4);
    gl.uniform1f(renderer.uniforms.shapeMorph, shapeMorph);
    gl.uniform1f(renderer.uniforms.movementAmplitude, movementAmplitude);
    gl.uniform1f(renderer.uniforms.dispersion, dispersion);
    gl.uniform1f(renderer.uniforms.frameRandomness, frameRandomness);
    gl.uniform1f(renderer.uniforms.widthVariety, widthVariety);
    gl.uniform1f(renderer.uniforms.cameraScale, cameraScale);
    gl.uniform1i(renderer.uniforms.useCameraTexture, useCameraTexture ? 1 : 0);

    if (useCameraTexture) {
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, renderer.cameraFrameTexture);
        gl.uniform1i(renderer.uniforms.cameraTexture, 0);
    }

    for (let i = 0; i < drawCount; i++) {
        let wing = wings[i];

        gl.uniform1f(renderer.uniforms.theta, theta + wing.phaseOffset * phaseSpread);
        gl.uniform1f(renderer.uniforms.side, wing.side);
        gl.uniform3f(renderer.uniforms.origin, wing.origin.x, wing.origin.y, wing.origin.z);
        gl.uniform1f(renderer.uniforms.lengthScale, lerp(1, wing.lengthScale, lengthVariety));
        gl.uniform1f(renderer.uniforms.bendScale, lerp(1, wing.bendScale, bendVariety));
        gl.uniform1f(renderer.uniforms.depthScale, lerp(1, wing.depthScale, depthVariety));
        gl.uniform2f(renderer.uniforms.textureAnchor, wing.textureAnchor.x, wing.textureAnchor.y);
        gl.uniform1f(renderer.uniforms.textureRotation, wing.textureRotation);
        gl.uniform1f(renderer.uniforms.textureScale, wing.textureScale);
        gl.uniform3fv(renderer.uniforms.cylinderColor, wing.cylinderColor);
        gl.uniform4fv(renderer.uniforms.cylinderWidthProfile, wing.cylinderWidthProfile);
        gl.uniform1f(renderer.uniforms.cylinderWaveAmount, wing.cylinderWaveAmount);
        gl.drawElements(gl.TRIANGLES, renderer.indexCount, gl.UNSIGNED_SHORT, 0);
    }

    gl.disableVertexAttribArray(renderer.attributes.joint);
    gl.disableVertexAttribArray(renderer.attributes.sideAngle);
    gl.disableVertexAttribArray(renderer.attributes.radiusScale);
}

// ─────────────────────────────────────
function createCylinderWidthProfile() {
    let rootWidth = random(12, 46);
    let middleWidth = random(10, 38);
    let tipWidth = random(5, 24);
    let wavePhase = random(TWO_PI);
    let waveAmount = random(0.06, 0.24);

    return {
        cylinderWidthProfile: new Float32Array([rootWidth, middleWidth, tipWidth, wavePhase]),
        cylinderWaveAmount: waveAmount,
    };
}

// ─────────────────────────────────────
function getShapeMorph() {
    if (!shapeMorphInput) return 0;

    return Number(shapeMorphInput.value) / 100;
}

// ─────────────────────────────────────
function getMovementAmplitude() {
    if (!movementAmplitudeInput) return 1;

    return Number(movementAmplitudeInput.value) / 25;
}

// ─────────────────────────────────────
function getDispersion() {
    if (!dispersionInput) return 1;

    return Number(dispersionInput.value) / 100;
}

// ─────────────────────────────────────
function getFrameRandomness() {
    if (!frameRandomnessInput) return 0;

    return Number(frameRandomnessInput.value) / 100;
}

// ─────────────────────────────────────
function getPhaseSpread() {
    if (!phaseSpreadInput) return 1;

    return Number(phaseSpreadInput.value) / 100;
}

// ─────────────────────────────────────
function getLengthVariety() {
    if (!lengthVarietyInput) return 1;

    return Number(lengthVarietyInput.value) / 100;
}

// ─────────────────────────────────────
function getWidthVariety() {
    if (!widthVarietyInput) return 1;

    return Number(widthVarietyInput.value) / 100;
}

// ─────────────────────────────────────
function getDensityCount() {
    if (!densityInput) return wings.length;

    return constrain(floor(Number(densityInput.value)), 0, wings.length);
}

// ─────────────────────────────────────
function getMotionSpeed() {
    if (!motionSpeedInput) return 1;

    return Number(motionSpeedInput.value) / 100;
}

// ─────────────────────────────────────
function getBendVariety() {
    if (!bendVarietyInput) return 1;

    return Number(bendVarietyInput.value) / 100;
}

// ─────────────────────────────────────
function getDepthVariety() {
    if (!depthVarietyInput) return 1;

    return Number(depthVarietyInput.value) / 100;
}

// ─────────────────────────────────────
function getCameraScale() {
    if (!cameraScaleInput) return 1;

    return Number(cameraScaleInput.value) / 100;
}

// ─────────────────────────────────────
function setCylinderAttribute(gl, location, strideCount, offsetCount) {
    if (location < 0) return;

    gl.enableVertexAttribArray(location);
    gl.vertexAttribPointer(location, 1, gl.FLOAT, false, strideCount * Float32Array.BYTES_PER_ELEMENT, offsetCount * Float32Array.BYTES_PER_ELEMENT);
}

// ─────────────────────────────────────
function createCameraFrameTexture(gl) {
    let texture = gl.createTexture();

    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texImage2D(
        gl.TEXTURE_2D,
        0,
        gl.RGBA,
        cameraMixWidth,
        cameraMixHeight,
        0,
        gl.RGBA,
        gl.UNSIGNED_BYTE,
        null,
    );

    return texture;
}

// ─────────────────────────────────────
function updateCameraFrameTexture(gl, texture, cameraTexture) {
    if (!cameraTexture) return false;

    let sourceCanvas = cameraTexture.canvas || cameraTexture.elt;
    if (!sourceCanvas) return false;

    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, gl.RGBA, gl.UNSIGNED_BYTE, sourceCanvas);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
    return true;
}

// ─────────────────────────────────────
function createGlProgram(gl, vertexSource, fragmentSource) {
    let vertexShader = compileGlShader(gl, gl.VERTEX_SHADER, vertexSource);
    let fragmentShader = compileGlShader(gl, gl.FRAGMENT_SHADER, fragmentSource);
    let program = gl.createProgram();

    gl.attachShader(program, vertexShader);
    gl.attachShader(program, fragmentShader);
    gl.linkProgram(program);

    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
        throw new Error(`Cylinder shader link failed: ${gl.getProgramInfoLog(program)}`);
    }

    return program;
}

// ─────────────────────────────────────
function compileGlShader(gl, type, source) {
    let shader = gl.createShader(type);

    gl.shaderSource(shader, source);
    gl.compileShader(shader);

    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
        throw new Error(`Cylinder shader compile failed: ${gl.getShaderInfoLog(shader)}`);
    }

    return shader;
}

// ─────────────────────────────────────
function cylinderVertexShader() {
    return `
precision highp float;

const int SEGMENT_COUNT = 20;
const float PI = 3.141592653589793;
const float TWO_PI = 6.283185307179586;
const float Y_TO_ANGLE = 0.012;
const float CENTER_CRANK_RADIUS = 28.0;
const float CAMERA_MIX_WIDTH = 640.0;
const float CAMERA_MIX_HEIGHT = 480.0;

attribute float aJoint;
attribute float aSideAngle;
attribute float aRadiusScale;

uniform mat4 uProjectionMatrix;
uniform mat4 uModelViewMatrix;
uniform float uTheta;
uniform float uSide;
uniform vec3 uOrigin;
uniform vec2 uTextureAnchor;
uniform float uTextureRotation;
uniform float uTextureScale;
uniform vec3 uCylinderColor;
uniform float uShapeMorph;
uniform float uMovementAmplitude;
uniform float uDispersion;
uniform float uFrameRandomness;
uniform float uLengthScale;
uniform float uBendScale;
uniform float uDepthScale;
uniform float uWidthVariety;
uniform float uCameraScale;
uniform vec4 uCylinderWidthProfile;
uniform float uCylinderWaveAmount;
uniform float uSegmentLengths[SEGMENT_COUNT];
uniform float uCrankRadii[SEGMENT_COUNT];
uniform float uCrankPhases[SEGMENT_COUNT];

varying vec2 vUv;
varying vec3 vColor;
varying float vShade;

float cylinderWidth(float t) {
    float rootWidth = mix(28.0, uCylinderWidthProfile.x, uWidthVariety);
    float middleWidth = mix(22.0, uCylinderWidthProfile.y, uWidthVariety);
    float tipWidth = mix(10.0, uCylinderWidthProfile.z, uWidthVariety);
    float wavePhase = uCylinderWidthProfile.w;
    float taperedWidth = mix(rootWidth, middleWidth, min(t * 2.0, 1.0));

    if (t > 0.5) {
        taperedWidth = mix(middleWidth, tipWidth, (t - 0.5) * 2.0);
    }

    return max(2.0, taperedWidth * (1.0 + sin(wavePhase + t * TWO_PI * 2.0) * uCylinderWaveAmount * uWidthVariety));
}

vec2 jointPosition(float jointIndex, float baseAngle, vec3 origin) {
    vec2 position = vec2(origin.x, origin.y + sin(uTheta) * CENTER_CRANK_RADIUS * uMovementAmplitude * uBendScale);
    float currentAngle = baseAngle;

    for (int i = 0; i < SEGMENT_COUNT; i++) {
        if (float(i) < jointIndex) {
            currentAngle += sin(uTheta + uCrankPhases[i]) * uCrankRadii[i] * Y_TO_ANGLE * uMovementAmplitude * uBendScale;
            position += vec2(cos(currentAngle), sin(currentAngle)) * uSegmentLengths[i] * uLengthScale;
        }
    }

    return position;
}

float tangentAngle(float jointIndex, float baseAngle) {
    float currentAngle = baseAngle;
    float lastJoint = min(jointIndex, float(SEGMENT_COUNT - 1));

    for (int i = 0; i < SEGMENT_COUNT; i++) {
        if (float(i) <= lastJoint) {
            currentAngle += sin(uTheta + uCrankPhases[i]) * uCrankRadii[i] * Y_TO_ANGLE * uMovementAmplitude * uBendScale;
        }
    }

    return currentAngle;
}

void main() {
    vec3 origin = vec3(uOrigin.xy * uDispersion, uOrigin.z * uDispersion * uDepthScale);
    float baseAngle = uSide > 0.0 ? 0.0 : PI;
    float t = aJoint / float(SEGMENT_COUNT);
    float morph = smoothstep(0.0, 1.0, uShapeMorph);
    float rootFade = smoothstep(0.0, 0.12, t);
    float tipFade = 1.0 - smoothstep(0.74, 1.0, t);
    float sideWidth = mix(0.1, 0.72, morph);
    float sideDepth = mix(0.001, 0.16, morph);
    float edgeSoftness = mix(0.36, 0.8, morph);
    float sideShape = pow(abs(cos(aSideAngle)), edgeSoftness);
    float radius = max(1.0, cylinderWidth(t) * rootFade * tipFade * 0.5) * aRadiusScale;
    float angle = tangentAngle(aJoint, baseAngle);
    vec2 normal = vec2(-sin(angle), cos(angle));
    vec2 position = jointPosition(aJoint, baseAngle, origin);
    vec3 worldPosition = vec3(
        position + normal * cos(aSideAngle) * radius * sideWidth * sideShape,
        origin.z + sin(aSideAngle) * radius * sideDepth
    );

    vec2 stableUv = worldPosition.xy * uCameraScale + vec2(CAMERA_MIX_WIDTH * 0.5, CAMERA_MIX_HEIGHT * 0.5);
    vec2 local = worldPosition.xy - origin.xy;
    float textureCos = cos(uTextureRotation);
    float textureSin = sin(uTextureRotation);
    vec2 rotated = vec2(
        local.x * textureCos - local.y * textureSin,
        local.x * textureSin + local.y * textureCos
    );
    vec2 randomUv = uTextureAnchor * vec2(CAMERA_MIX_WIDTH, CAMERA_MIX_HEIGHT) + rotated * uTextureScale * uCameraScale;

    vUv = mod(mix(stableUv, randomUv, uFrameRandomness), vec2(CAMERA_MIX_WIDTH, CAMERA_MIX_HEIGHT));
    vColor = uCylinderColor;
    vShade = mix(1.0, 0.82 + 0.18 * max(0.0, sin(aSideAngle) * 0.5 + 0.5), morph);
    gl_Position = uProjectionMatrix * uModelViewMatrix * vec4(worldPosition, 1.0);
}
`;
}

// ─────────────────────────────────────
function cylinderFragmentShader() {
    return `
precision mediump float;

uniform sampler2D uCameraTexture;
uniform int uUseCameraTexture;

varying vec2 vUv;
varying vec3 vColor;
varying float vShade;

void main() {
    vec3 color = vColor;

    if (uUseCameraTexture == 1) {
        vec2 uv = vec2(vUv.x / 640.0, 1.0 - vUv.y / 480.0);
        color = texture2D(uCameraTexture, uv).rgb;
    }

    gl_FragColor = vec4(color * vShade, 1.0);
}
`;
}
