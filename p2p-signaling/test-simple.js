const WebSocket = require('ws');
const ws = new WebSocket('wss://p2p.charlesneimog.workers.dev/?room=teste');

ws.on('open', () => {
    console.log('✅ WEBSOCKET CONECTADO COM SUCESSO!');
    ws.send(JSON.stringify({ type: 'join', name: 'Joao' }));
});

ws.on('message', (data) => {
    const msg = JSON.parse(data);
    console.log('📨 Mensagem recebida:', msg);
    if (msg.type === 'welcome') {
        console.log('✅ Conexão estabelecida! ID:', msg.id);
        process.exit(0);
    }
});

ws.on('error', (e) => console.error('❌ Erro:', e.message));

setTimeout(() => {
    console.log('Finalizando...');
    process.exit(0);
}, 3000);
