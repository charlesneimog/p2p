# P2P Signaling Server (WebRTC)

A WebRTC signaling server built with Cloudflare Workers and Durable Objects to manage peer-to-peer connection rooms.

## Prerequisites

1. **Node.js** installed on your machine (version 26 or later recommended).
2. An active **Cloudflare** account.

## Installation

Project management is handled through **Wrangler**, Cloudflare's official CLI.
1. Install the dependencies globally (optional; you can also use it through `npx`):

```bash
npm install -g wrangler
```

2. Log in to your Cloudflare account from the terminal:

```bash
wrangler login
```

*This will open a browser window where you can authorize access.*

## Running Locally (Development)

To test the signaling server on your local machine before deploying it to the cloud, run:

```bash
npx wrangler dev
```

or, if you installed Wrangler globally:

```bash
wrangler dev
```

Press `b` in the terminal to open the browser, or access the local URL shown in the console (usually `http://localhost:8787`).

To test WebSocket connections from a local frontend, change the URL in your client to:

```text
ws://localhost:8787?room=your-room
```

## Deploying to Production

When the code is ready for production, deployment will upload the Worker and provision the Durable Objects across Cloudflare's global network.

Run:

```bash
npx wrangler deploy
```

After deployment, the terminal will display the production URL (for example, `https://p2p-signaling.<your-username>.workers.dev`).
Use the `wss://` version of this URL in your frontend client.

## File Structure

* `index.js`: Worker entry point; handles routing and room name extraction.
* `room.js`: Durable Object logic; responsible for maintaining active WebSocket connections and broadcasting SDP (Session Description Protocol) and ICE candidates.
* `wrangler.toml`: Cloudflare configuration and infrastructure provisioning file.

