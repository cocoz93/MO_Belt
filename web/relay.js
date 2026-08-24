// Protocol-agnostic WS <-> TCP relay (replaces websocat; no external install).
// Browser connects via WebSocket; relay opens a TCP socket to the game server and
// pipes raw bytes both ways. Packet framing is handled by the client, not here.
//
// Usage: node relay.js [--ws 9000] [--server-host 127.0.0.1] [--server-port 6000] [--log]
const net = require('net');
const { WebSocketServer } = require('ws');

function arg(name, def){ const i=process.argv.indexOf('--'+name); return i>=0 ? process.argv[i+1] : def; }
const WS_PORT      = parseInt(arg('ws', '9000'), 10);
const SERVER_HOST  = arg('server-host', '127.0.0.1');
const SERVER_PORT  = parseInt(arg('server-port', '6000'), 10);  // game server default (launchers pass it explicitly)
const LOG          = process.argv.includes('--log');

const wss = new WebSocketServer({ host: '127.0.0.1', port: WS_PORT });
console.log(`[relay] WS ws://127.0.0.1:${WS_PORT}  ->  TCP ${SERVER_HOST}:${SERVER_PORT}`);

let idc = 0;
wss.on('connection', (ws) => {
  const id = ++idc;
  // [Belt] WS 쪽 다리에도 NODELAY 명시 — 원본은 TCP 쪽만 걸고 WS 쪽은 ws 기본값에 기대고 있었다
  if (ws._socket && ws._socket.setNoDelay) ws._socket.setNoDelay(true);
  const tcp = net.connect({ host: SERVER_HOST, port: SERVER_PORT }, () => {
    tcp.setNoDelay(true);
    if (LOG) console.log(`[relay#${id}] TCP connected to server`);
  });
  let up=0, down=0;
  // browser -> server
  ws.on('message', (data, isBinary) => {
    const buf = Buffer.isBuffer(data) ? data : Buffer.from(data);
    up += buf.length;
    if (tcp.writable) tcp.write(buf);
    if (LOG) console.log(`[relay#${id}] WS->TCP ${buf.length}B (total ${up})`);
  });
  // server -> browser
  tcp.on('data', (buf) => {
    down += buf.length;
    if (ws.readyState === ws.OPEN) ws.send(buf, { binary: true });
    if (LOG) console.log(`[relay#${id}] TCP->WS ${buf.length}B (total ${down})`);
  });
  const bye = (who) => { if (LOG) console.log(`[relay#${id}] closed by ${who} (up ${up} down ${down})`); try{tcp.destroy();}catch{} try{ws.close();}catch{} };
  ws.on('close', ()=>bye('ws')); ws.on('error', (e)=>{ if(LOG)console.log('[relay] ws err',e.message); bye('ws-err'); });
  tcp.on('close', ()=>bye('tcp')); tcp.on('error', (e)=>{ console.log(`[relay#${id}] TCP error: ${e.message}`); try{ws.close();}catch{} });
});
wss.on('error',(e)=>console.log('[relay] WS server error:', e.message));
