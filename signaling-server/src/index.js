import { Room } from "./room.js";

export { Room };

export default {
    async fetch(request, env) {
        const url = new URL(request.url);
        const roomName = url.searchParams.get("room") || "default";
        const id = env.ROOMS.idFromName(roomName);
        const room = env.ROOMS.get(id);
        return room.fetch(request);
    },
};
