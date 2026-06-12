precision mediump float;

uniform sampler2D uVideo;
uniform float uTime;
uniform float uNoiseAmount;

varying vec2 vTexCoord;

float hash(vec2 p) {
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
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
    float v = 0.0;
    float a = 0.5;

    for (int i = 0; i < 5; i++) {
        v += noise(p) * a;
        p *= 2.15;
        a *= 0.5;
    }

    return v;
}

void main() {
    vec2 uv = vTexCoord;
    float amount = clamp(uNoiseAmount, 0.0, 1.0);
    float t = uTime;

    vec2 center = vec2(0.5);
    vec2 p = uv - center;
    float dist = length(p);

    // pseudo-3D radial depth field
    float depth = fbm(uv * 4.0 + vec2(t * 0.15, -t * 0.1));
    depth += 0.45 * sin(dist * 18.0 - t * 3.0);
    depth = depth * 0.5 + 0.5;

    // digital flow field
    vec2 flow;
    flow.x = fbm(uv * 6.0 + vec2(t * 0.35, 0.0));
    flow.y = fbm(uv * 6.0 + vec2(0.0, -t * 0.35));
    flow = flow * 2.0 - 1.0;

    // motion through a noisy 3D-like space
    vec2 warpedUv = uv;
    warpedUv += normalize(p + 0.0001) * depth * amount * 0.06;
    warpedUv += flow * amount * 0.035;

    // tunnel / depth pulse
    float pulse = sin(dist * 28.0 - t * 5.0);
    warpedUv += normalize(p + 0.0001) * pulse * amount * 0.015;

    // depth-based RGB separation
    float split = amount * (0.006 + depth * 0.025);

    vec3 col;
    col.r = texture2D(uVideo, warpedUv + flow * split).r;
    col.g = texture2D(uVideo, warpedUv).g;
    col.b = texture2D(uVideo, warpedUv - flow * split).b;

    // motion echo / temporal smear approximation
    vec3 trail1 = texture2D(uVideo, warpedUv - flow * amount * 0.025).rgb;
    vec3 trail2 = texture2D(uVideo, warpedUv - flow * amount * 0.055).rgb;
    col = mix(col, trail1, amount * 0.25);
    col = mix(col, trail2, amount * 0.15);

    // digital particle noise
    vec2 grid = floor(uv * vec2(160.0, 90.0));
    float sparkle = hash(grid + floor(t * 30.0));
    float particle = step(0.985 - amount * 0.03, sparkle);

    col += particle * vec3(0.25, 0.65, 1.0) * amount;

    // scan-depth lines, more synthetic than VHS
    float lines = sin((uv.y + depth * 0.04) * 900.0);
    col *= 0.92 + 0.08 * lines * amount;

    // contrast and luminous digital color
    col = pow(col, vec3(0.85));
    col += depth * amount * vec3(0.04, 0.08, 0.16);

    // slight quantized digital edge
    float levels = mix(256.0, 32.0, amount);
    col = floor(col * levels) / levels;

    gl_FragColor = vec4(col, 1.0);
}
