let remoteVideoSource;
let sketchContainer;
let noiseAmountInput;

let videoBuffer;
let effectShader;

const vertexShaderSource = `
attribute vec3 aPosition;
attribute vec2 aTexCoord;

uniform mat4 uModelViewMatrix;
uniform mat4 uProjectionMatrix;

varying vec2 vTexCoord;

void main() {
    vTexCoord = aTexCoord;
    gl_Position = uProjectionMatrix * uModelViewMatrix * vec4(aPosition, 1.0);
}
`;

const fragmentShaderSource = `
precision highp float;

uniform sampler2D uVideo;
uniform vec2 uResolution;
uniform float uTime;
uniform float uNoiseAmount;

varying vec2 vTexCoord;

float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);

    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));

    vec2 u = f * f * (3.0 - 2.0 * f);

    return mix(a, b, u.x)
        + (c - a) * u.y * (1.0 - u.x)
        + (d - b) * u.x * u.y;
}

float fbm(vec2 p) {
    float value = 0.0;
    float amplitude = 0.5;

    for (int i = 0; i < 4; i++) {
        value += noise(p) * amplitude;
        p *= 2.05;
        amplitude *= 0.5;
    }

    return value;
}

vec2 flowAt(vec2 uv, float t, float amount) {
    float base = noise(uv * 9.0 + vec2(t * 0.12, -t * 0.08)) * 6.2831853;
    float quantized = floor(base / 6.2831853 * 90.0) / 90.0 * 6.2831853;

    vec3 cam = texture2D(uVideo, uv).rgb;
    float colorPush = (cam.r - cam.b) * 3.14159265;
    float angle = mix(quantized, base * 2.8 + colorPush, amount * 0.65);

    vec2 flow = vec2(cos(angle), sin(angle));
    vec2 turbulence = vec2(
        fbm(uv.yx * 18.0 + vec2(t * 0.25, 0.0)),
        fbm(uv.xy * 18.0 + vec2(0.0, -t * 0.25))
    ) * 2.0 - 1.0;

    return normalize(flow + turbulence * amount * 0.9);
}

void main() {
    vec2 uv = vTexCoord;
    uv.y = 1.0 - uv.y;

    float amount = clamp(uNoiseAmount, 0.0, 1.0);
    float t = uTime;
    vec3 rawCamera = texture2D(uVideo, uv).rgb;

    vec2 cellCount = mix(vec2(220.0, 124.0), vec2(420.0, 236.0), amount);
    vec2 cell = floor(uv * cellCount);
    vec2 cellUv = (cell + 0.5) / cellCount;
    vec2 inCell = fract(uv * cellCount) - 0.5;

    float seed = hash(cell);
    float age = fract(seed + t * mix(0.55, 1.35, amount));

    vec2 flow = flowAt(cellUv, t, amount);
    vec2 driftUv = cellUv - flow * age * mix(0.012, 0.08, amount);
    driftUv += (vec2(hash(cell + 13.7), hash(cell + 91.1)) - 0.5) / cellCount * amount * 1.6;
    driftUv = clamp(driftUv, vec2(0.001), vec2(0.999));

    vec3 cam = texture2D(uVideo, driftUv).rgb;
    float brightness = dot(cam, vec3(0.333333));

    float brightBias = smoothstep(0.03, 0.62, brightness);
    float spawn = step(1.0 - mix(0.48, 0.92, amount) * brightBias, seed);

    vec2 sideDir = vec2(-flow.y, flow.x);
    float wobble = sin(t * mix(2.4, 5.2, amount) + seed * 43.0);
    vec2 movingCenter = flow * (age - 0.5) * mix(0.65, 1.35, amount);
    movingCenter += sideDir * wobble * mix(0.04, 0.16, amount);

    vec2 movingCell = inCell - movingCenter;
    vec2 echoCell = inCell - movingCenter + flow * mix(0.18, 0.42, amount);

    float radius = mix(0.11, 0.28, brightness) * mix(0.9, 1.2, amount);
    float particle = smoothstep(radius, radius * 0.16, length(movingCell));
    float echo = smoothstep(radius * 1.25, radius * 0.22, length(echoCell)) * 0.36;

    vec2 tailDir = normalize(flow + 0.0001);
    float tail = dot(movingCell, -tailDir);
    float cross = abs(dot(movingCell, vec2(-tailDir.y, tailDir.x)));
    float streak = smoothstep(0.09, 0.0, cross) * smoothstep(-0.16, 0.58, tail);
    streak *= smoothstep(0.62, 0.0, length(movingCell));

    float body = max(max(particle, echo), streak * mix(0.45, 1.0, amount));
    float fadeInOut = smoothstep(0.0, 0.18, age) * smoothstep(1.0, 0.72, age);
    float alpha = spawn * body * fadeInOut * mix(0.6, 1.0, brightBias);

    vec3 trailCam = texture2D(uVideo, clamp(driftUv - flow * 0.025 * amount, vec2(0.001), vec2(0.999))).rgb;
    vec3 color = mix(cam, trailCam, amount * 0.28);

    float contrast = mix(1.08, 1.35, amount);
    color = pow(color, vec3(0.82));
    color *= contrast;

    vec3 background = rawCamera * mix(0.92, 0.42, amount);
    vec3 finalColor = mix(background, color, alpha);

    float softCameraGhost = brightness * smoothstep(0.75, 0.0, length(movingCell)) * 0.025;
    finalColor += cam * softCameraGhost * amount;

    float grain = hash(uv * uResolution + floor(t * 60.0)) - 0.5;
    float scan = sin((uv.y + fbm(uv * 12.0 + t * 0.25) * 0.035) * uResolution.y * 1.6);
    float tear = step(0.992 - amount * 0.08, hash(vec2(floor(uv.y * 80.0), floor(t * 18.0))));
    vec2 tearUv = clamp(uv + vec2((hash(vec2(floor(uv.y * 80.0), t)) - 0.5) * amount * tear * 0.08, 0.0), vec2(0.001), vec2(0.999));
    vec3 tornCamera = texture2D(uVideo, tearUv).rgb;

    finalColor = mix(finalColor, tornCamera, tear * amount * 0.75);
    finalColor += grain * amount * 0.22;
    finalColor *= 1.0 + scan * amount * 0.16;

    gl_FragColor = vec4(mix(rawCamera, finalColor, amount), 1.0);
}
`;

// ─────────────────────────────────────
function setup() {
    sketchContainer = document.getElementById("sketch-container");
    noiseAmountInput = document.getElementById("noise-amount");

    pixelDensity(1);

    const sketchSize = getSketchSize();
    const canvas = createCanvas(sketchSize.width, sketchSize.height, WEBGL);
    canvas.parent(sketchContainer);

    noStroke();
    textureMode(NORMAL);

    videoBuffer = createGraphics(width, height);
    videoBuffer.pixelDensity(1);

    effectShader = createShader(vertexShaderSource, fragmentShaderSource);
}

// ─────────────────────────────────────
function draw() {
    remoteVideoSource = getRemoteSource();

    if (!remoteVideoSource) {
        resetShader();
        background(0);
        return;
    }

    if (!updateVideoBuffer()) {
        resetShader();
        background(0);
        return;
    }

    shader(effectShader);
    effectShader.setUniform("uVideo", videoBuffer);
    effectShader.setUniform("uResolution", [width, height]);
    effectShader.setUniform("uTime", millis() / 1000);
    effectShader.setUniform("uNoiseAmount", getNoiseAmount());

    plane(width, height);
}

// ─────────────────────────────────────
function updateVideoBuffer() {
    if (!remoteVideoSource || !videoBuffer || width <= 0 || height <= 0) return false;

    videoBuffer.background(0);

    const sourceW = remoteVideoSource.videoWidth || remoteVideoSource.naturalWidth || remoteVideoSource.width;
    const sourceH = remoteVideoSource.videoHeight || remoteVideoSource.naturalHeight || remoteVideoSource.height;

    if (!sourceW || !sourceH) return false;

    const canvasAspect = width / height;
    const videoAspect = sourceW / sourceH;

    let drawW = width;
    let drawH = height;
    let drawX = 0;
    let drawY = 0;

    if (canvasAspect > videoAspect) {
        drawW = height * videoAspect;
        drawX = (width - drawW) * 0.5;
    } else {
        drawH = width / videoAspect;
        drawY = (height - drawH) * 0.5;
    }

    videoBuffer.drawingContext.drawImage(remoteVideoSource, drawX, drawY, drawW, drawH);

    return true;
}

// ─────────────────────────────────────
function windowResized() {
    if (!sketchContainer) return;

    const sketchSize = getSketchSize();
    resizeCanvas(sketchSize.width, sketchSize.height);

    videoBuffer = createGraphics(width, height);
    videoBuffer.pixelDensity(1);
}

// ─────────────────────────────────────
function getSketchSize() {
    return {
        width: Math.max(1, Math.floor(sketchContainer?.clientWidth || windowWidth || 1)),
        height: Math.max(1, Math.floor(sketchContainer?.clientHeight || windowHeight || 1)),
    };
}

// ─────────────────────────────────────
function getNoiseAmount() {
    const value = Number.parseFloat(noiseAmountInput?.value);
    return Number.isFinite(value) ? constrain(value, 0, 1) : 0.7;
}

// ─────────────────────────────────────
function getRemoteSource() {
    const mixerSource = window.remoteVideoMixer?.source;
    const domSource = document.getElementById("remote-mix");

    const source = mixerSource || domSource;

    if (!source) return null;

    if (source instanceof HTMLVideoElement) {
        if (source.readyState < 2 || source.videoWidth <= 0 || source.videoHeight <= 0) {
            return null;
        }
        return source;
    }

    if (source instanceof HTMLCanvasElement) {
        if (source.width <= 0 || source.height <= 0) {
            return null;
        }
        return source;
    }

    if (source instanceof HTMLImageElement) {
        if (!source.complete || source.naturalWidth <= 0 || source.naturalHeight <= 0) {
            return null;
        }
        return source;
    }

    return null;
}
