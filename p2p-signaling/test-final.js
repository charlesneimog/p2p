const WebSocket = require('ws');
const ws = new WebSocket('wss://p2p.charlesneimog.workers.dev/?room=teste');

ws.on('open', () => {
    console.log('✅ WEBSOCKET CONECTADO!');
    ws.send(JSON.stringify({ type: 'join', name: 'Teste' }));
});

ws.on('message', (data) => {
    const msg = JSON.parse(data);
    console.log('📨 Recebido:', msg);
    if (msg.type === 'welcome') {
        console.log('✅ Conexão estabelecida com sucesso!');
        process.exit(0);
    }
});

ws.on('error', (e) => {
    console.error('❌ Erro:', e.message);
    process.exit(1);
});

setTimeout(() => {
    console.error('❌ Timeout - WebSocket não conectou');
    process.exit(1);
}, 5000);
