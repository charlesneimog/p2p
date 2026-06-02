const WebSocket = require('ws');
const ws = new WebSocket('wss://p2p.charlesneimog.workers.dev/?room=teste');

ws.on('open', () => {
    console.log('✅ WEBSOCKET CONECTADO!');
    ws.send(JSON.stringify({ type: 'join', name: 'Joao' }));
});

ws.on('error', (e) => console.error('❌ Erro:', e.message));

ws.on('message', (data) => {
    const msg = JSON.parse(data);
    console.log('📨 Mensagem recebida:', msg);
});

ws.on('close', () => console.log('🔌 Conexão fechada'));

setTimeout(() => {
    console.log('Finalizando teste...');
    process.exit(0);
}, 3000);
