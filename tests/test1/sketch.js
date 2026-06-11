let remoteVideoP5;
let sketchContainer;
let gl;
let shaderProgram;
let videoTexture;
let positionBuffer;
let texCoordBuffer;

const vertexShaderSource = `
attribute vec2 aPosition;
attribute vec2 aTexCoord;

varying vec2 vTexCoord;

void main() {
    vTexCoord = aTexCoord;
    gl_Position = vec4(aPosition, 0.0, 1.0);
}
`;

const fragmentShaderSource = `
precision mediump float;

uniform sampler2D uVideo;
uniform float uTime;
uniform float uNoiseAmount;
varying vec2 vTexCoord;

float random(vec2 st) {
    return fract(sin(dot(st.xy, vec2(12.9898, 78.233))) * 43758.5453123);
}

float noise(vec2 st) {
    vec2 i = floor(st);
    vec2 f = fract(st);

    float a = random(i);
    float b = random(i + vec2(1.0, 0.0));
    float c = random(i + vec2(0.0, 1.0));
    float d = random(i + vec2(1.0, 1.0));

    vec2 u = f * f * (3.0 - 2.0 * f);

    return mix(a, b, u.x) +
        (c - a) * u.y * (1.0 - u.x) +
        (d - b) * u.x * u.y;
}

float fbm(vec2 st) {
    float value = 0.0;
    float amplitude = 0.5;

    for (int i = 0; i < 5; i++) {
        value += amplitude * noise(st);
        st *= 2.0;
        amplitude *= 0.5;
    }

    return value;
}

void main() {
    vec2 center = vTexCoord - 0.5;
    float radius = length(center);
    float angle = atan(center.y, center.x);
    float pulse = sin(uTime * 2.4 + radius * 22.0) * 0.5 + 0.5;
    float warpedAngle = angle + fbm(vTexCoord * 3.0 + uTime * 0.12) * uNoiseAmount * 2.4;
    float warpedRadius = radius + sin(angle * 8.0 + uTime * 1.7) * 0.018 * uNoiseAmount;
    vec2 warpedUv = vec2(cos(warpedAngle), sin(warpedAngle)) * warpedRadius + 0.5;
    vec2 wave = vec2(
        sin(vTexCoord.y * 38.0 + uTime * 4.0),
        cos(vTexCoord.x * 31.0 - uTime * 3.3)
    ) * 0.012 * uNoiseAmount;
    warpedUv += wave;

    vec2 chroma = normalize(center + 0.0001) * (0.012 + pulse * 0.018) * uNoiseAmount;
    float red = texture2D(uVideo, warpedUv + chroma).r;
    float green = texture2D(uVideo, warpedUv).g;
    float blue = texture2D(uVideo, warpedUv - chroma).b;
    vec4 color = vec4(red, green, blue, 1.0);

    float grain = random(vTexCoord * 1200.0 + uTime * 35.0) - 0.5;
    float cloud = fbm(vTexCoord * 8.0 + vec2(uTime * 0.2, -uTime * 0.14));
    vec3 hotTint = vec3(1.25, 0.35 + pulse * 0.45, 0.18);
    vec3 coldTint = vec3(0.08, 0.35, 1.35);
    vec3 tint = mix(coldTint, hotTint, cloud);
    float scanline = sin(vTexCoord.y * 900.0 + uTime * 14.0) * 0.5 + 0.5;

    color.rgb += grain * uNoiseAmount * 1.5;
    color.rgb *= mix(0.72, 1.22, scanline);
    color.rgb = mix(color.rgb, color.rgb * tint, uNoiseAmount * 0.95);
    color.rgb = floor(color.rgb * 7.0) / 7.0;
    color.rgb += vec3(
        sin(uTime * 4.1 + vTexCoord.x * 18.0),
        sin(uTime * 5.3 + vTexCoord.y * 22.0),
        sin(uTime * 3.7 + radius * 40.0)
    ) * 0.08 * uNoiseAmount;
    color.rgb = clamp(color.rgb, 0.0, 1.0);

    gl_FragColor = color;
}
`;

function setup() {
    sketchContainer = document.getElementById("sketch-container");
    remoteVideoP5 = document.getElementById("remote-video");

    pixelDensity(1);
    const canvas = createCanvas(sketchContainer.clientWidth, sketchContainer.clientHeight, WEBGL);
    canvas.parent(sketchContainer);

    gl = drawingContext;

    initGpuVideoRenderer();
}

function draw() {
    if (!remoteVideoP5 || remoteVideoP5.readyState < 2) {
        if (gl) {
            gl.clearColor(0, 0, 0, 1);
            gl.clear(gl.COLOR_BUFFER_BIT);
        }
        return;
    }

    drawVideoFrame();
}

function windowResized() {
    if (!sketchContainer) return;
    resizeCanvas(sketchContainer.clientWidth, sketchContainer.clientHeight);
    setGpuViewport();
}

function initGpuVideoRenderer() {
    shaderProgram = createGpuProgram(vertexShaderSource, fragmentShaderSource);

    positionBuffer = gl.createBuffer();

    texCoordBuffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, texCoordBuffer);
    gl.bufferData(
        gl.ARRAY_BUFFER,
        new Float32Array([
            0, 1,
            1, 1,
            0, 0,
            0, 0,
            1, 1,
            1, 0,
        ]),
        gl.STATIC_DRAW,
    );

    videoTexture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, videoTexture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
}

function drawVideoFrame() {
    setGpuViewport();
    updateVideoQuad();

    gl.clearColor(0, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);

    gl.useProgram(shaderProgram);

    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, videoTexture);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, remoteVideoP5);

    const videoLocation = gl.getUniformLocation(shaderProgram, "uVideo");
    gl.uniform1i(videoLocation, 0);
    gl.uniform1f(gl.getUniformLocation(shaderProgram, "uTime"), millis() / 1000);
    gl.uniform1f(gl.getUniformLocation(shaderProgram, "uNoiseAmount"), 0.7);

    bindAttribute("aPosition", positionBuffer, 2);
    bindAttribute("aTexCoord", texCoordBuffer, 2);

    gl.drawArrays(gl.TRIANGLES, 0, 6);
}

function setGpuViewport() {
    if (!gl) return;
    gl.viewport(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight);
}

function updateVideoQuad() {
    const videoWidth = remoteVideoP5.videoWidth || width;
    const videoHeight = remoteVideoP5.videoHeight || height;
    const canvasAspect = gl.drawingBufferWidth / gl.drawingBufferHeight;
    const videoAspect = videoWidth / videoHeight;
    let quadWidth = 1;
    let quadHeight = 1;

    if (canvasAspect > videoAspect) {
        quadWidth = videoAspect / canvasAspect;
    } else {
        quadHeight = canvasAspect / videoAspect;
    }

    gl.bindBuffer(gl.ARRAY_BUFFER, positionBuffer);
    gl.bufferData(
        gl.ARRAY_BUFFER,
        new Float32Array([
            -quadWidth, -quadHeight,
            quadWidth, -quadHeight,
            -quadWidth, quadHeight,
            -quadWidth, quadHeight,
            quadWidth, -quadHeight,
            quadWidth, quadHeight,
        ]),
        gl.DYNAMIC_DRAW,
    );
}

function bindAttribute(name, buffer, size) {
    const location = gl.getAttribLocation(shaderProgram, name);
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.enableVertexAttribArray(location);
    gl.vertexAttribPointer(location, size, gl.FLOAT, false, 0, 0);
}

function createGpuProgram(vertexSource, fragmentSource) {
    const vertexShader = compileGpuShader(gl.VERTEX_SHADER, vertexSource);
    const fragmentShader = compileGpuShader(gl.FRAGMENT_SHADER, fragmentSource);
    const program = gl.createProgram();

    gl.attachShader(program, vertexShader);
    gl.attachShader(program, fragmentShader);
    gl.linkProgram(program);

    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
        throw new Error(gl.getProgramInfoLog(program));
    }

    return program;
}

function compileGpuShader(type, source) {
    const shader = gl.createShader(type);

    gl.shaderSource(shader, source);
    gl.compileShader(shader);

    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
        throw new Error(gl.getShaderInfoLog(shader));
    }

    return shader;
}
