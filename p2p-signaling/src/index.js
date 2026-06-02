export default {
    async fetch(request, env) {
        const url = new URL(request.url);
        
        // Verificar se é WebSocket pelo método e headers
        const isWebSocket = request.headers.get('Upgrade')?.toLowerCase() === 'websocket';
        
        // Se for WebSocket, processar
        if (isWebSocket) {
            try {
                const webSocketPair = new WebSocketPair();
                const [client, server] = Object.values(webSocketPair);
                
                server.accept();
                console.log('✅ WebSocket connection accepted');
                
                // Responder a mensagens
                server.addEventListener('message', (event) => {
                    console.log('Message received:', event.data);
                    try {
                        const data = JSON.parse(event.data);
                        if (data.type === 'join') {
                            server.send(JSON.stringify({
                                type: 'welcome',
                                id: crypto.randomUUID()
                            }));
                        }
                    } catch (e) {
                        server.send(`Echo: ${event.data}`);
                    }
                });
                
                server.addEventListener('close', () => {
                    console.log('WebSocket closed');
                });
                
                return new Response(null, { status: 101, webSocket: client });
            } catch (err) {
                console.error('WebSocket error:', err);
                return new Response('WebSocket error', { status: 500 });
            }
        }
        
        // Health check para GET normal
        if (request.method === 'GET') {
            return new Response('✅ Signaling Server Online\nWebSocket endpoint: wss://p2p.charlesneimog.workers.dev/?room=NOME', {
                status: 200,
                headers: { 'Content-Type': 'text/plain' }
            });
        }
        
        return new Response('Method not allowed', { status: 405 });
    }
};
