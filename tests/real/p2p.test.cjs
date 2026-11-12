const assert = require("node:assert/strict");
const { readFileSync } = require("node:fs");
const { runInNewContext } = require("node:vm");
const { test } = require("node:test");

const source = readFileSync(`${__dirname}/p2p.js`, "utf8");
const SimpleP2P = runInNewContext(`${source}\nSimpleP2P`);

function client() {
    return new SimpleP2P("room", "browser");
}

test("Opus SDP receives stereo while advertising a mono sender", () => {
    const description = client()._withOpusChannelPreferences({
        type: "offer",
        sdp: [
            "v=0",
            "m=audio 9 UDP/TLS/RTP/SAVPF 111",
            "a=rtpmap:111 opus/48000/2",
            "a=fmtp:111 minptime=10;useinbandfec=1;stereo=0;sprop-stereo=1",
            "",
        ].join("\r\n"),
    });

    assert.match(
        description.sdp,
        /a=fmtp:111 minptime=10;useinbandfec=1;stereo=1;sprop-stereo=0\r\n/,
    );
});

test("Opus channel preferences add a missing fmtp line", () => {
    const description = client()._withOpusChannelPreferences({
        type: "answer",
        sdp: ["v=0", "m=audio 9 UDP/TLS/RTP/SAVPF 109", "a=rtpmap:109 opus/48000/2", ""].join("\r\n"),
    });

    assert.match(description.sdp, /a=rtpmap:109 opus\/48000\/2\r\na=fmtp:109 stereo=1;sprop-stereo=0\r\n/);
});

test("non-Opus SDP remains unchanged", () => {
    const original = {
        type: "offer",
        sdp: "v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 0\r\na=rtpmap:0 PCMU/8000\r\n",
    };

    assert.equal(client()._withOpusChannelPreferences(original), original);
});
