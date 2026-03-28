/*

- ═══════════════════════════════════════════════════════════════
- Mercedes W213 – Cloud Bridge Server
- Node.js + WebSocket
- 
- Deploy pe Railway/Render/Fly.io
- 
- Flux:
- iPhone → HTTP POST → Server → WebSocket → ESP32 → CAN Bus
- ESP32  → WebSocket → Server → HTTP GET  → iPhone
- ═══════════════════════════════════════════════════════════════
  */

const express   = require(‘express’);
const path      = require(‘path’);
const http      = require(‘http’);
const WebSocket = require(‘ws’);
const cors      = require(‘cors’);

const app    = express();
const server = http.createServer(app);
const wss    = new WebSocket.Server({ server });

app.use(cors());
app.use(express.json());
app.use(express.static(path.join(__dirname)));

// ─── State ────────────────────────────────────────────────────
let esp32Socket = null;   // conexiunea WebSocket a ESP32
let carState = {
connected: false,
engine:    false,
locked:    true,
ac_on:     false,
ac_temp:   22,
ac_fan:    3,
lastSeen:  null,
};

// Queue pentru comenzi in asteptare (daca ESP32 e offline)
let pendingCommands = [];

// ─── WebSocket – ESP32 se conecteaza aici ─────────────────────
wss.on(‘connection’, (ws, req) => {
const ip = req.socket.remoteAddress;
console.log(`[WS] ESP32 conectat de la: ${ip}`);

esp32Socket      = ws;
carState.connected = true;
carState.lastSeen  = new Date().toISOString();

// Trimite comenzile in asteptare
if (pendingCommands.length > 0) {
console.log(`[WS] Trimit ${pendingCommands.length} comenzi in asteptare`);
pendingCommands.forEach(cmd => ws.send(JSON.stringify(cmd)));
pendingCommands = [];
}

// Primeste status/date de la ESP32
ws.on(‘message’, (data) => {
try {
const msg = JSON.parse(data);
console.log(`[WS] Date de la ESP32:`, msg);
if (msg.type === ‘state’) {
carState = { …carState, …msg.data, connected: true, lastSeen: new Date().toISOString() };
}
} catch (e) {
console.error(’[WS] JSON invalid:’, e.message);
}
});

ws.on(‘close’, () => {
console.log(’[WS] ESP32 deconectat’);
esp32Socket        = null;
carState.connected = false;
});

ws.on(‘error’, (err) => {
console.error(’[WS] Eroare:’, err.message);
});

// Ping la fiecare 30s ca sa mentina conexiunea
const pingInterval = setInterval(() => {
if (ws.readyState === WebSocket.OPEN) {
ws.ping();
} else {
clearInterval(pingInterval);
}
}, 30000);
});

// ─── Helper: trimite comanda la ESP32 ─────────────────────────
function sendToESP32(command) {
if (esp32Socket && esp32Socket.readyState === WebSocket.OPEN) {
esp32Socket.send(JSON.stringify(command));
return true;
} else {
// Salveaza comanda pentru cand ESP32 se reconecteaza
pendingCommands.push(command);
console.log(`[QUEUE] Comanda salvata: ${command.action}`);
return false;
}
}

function apiResponse(res, sent, message) {
res.json({
status:    ‘ok’,
message,
delivered: sent,
queued:    !sent,
});
}

// ─── REST API – iPhone trimite comenzi aici ───────────────────

// GET /api/status
app.get(’/api/status’, (req, res) => {
res.json({
status:   ‘ok’,
server:   ‘online’,
esp32:    carState.connected ? ‘connected’ : ‘disconnected’,
lastSeen: carState.lastSeen,
car:      carState,
});
});

// POST /api/engine/start
app.post(’/api/engine/start’, (req, res) => {
const sent = sendToESP32({ action: ‘engine_start’ });
if (sent) carState.engine = true;
apiResponse(res, sent, sent ? ‘Motor pornit’ : ‘Comanda in asteptare’);
});

// POST /api/engine/stop
app.post(’/api/engine/stop’, (req, res) => {
const sent = sendToESP32({ action: ‘engine_stop’ });
if (sent) carState.engine = false;
apiResponse(res, sent, sent ? ‘Motor oprit’ : ‘Comanda in asteptare’);
});

// POST /api/locks/lock
app.post(’/api/locks/lock’, (req, res) => {
const sent = sendToESP32({ action: ‘lock’ });
if (sent) carState.locked = true;
apiResponse(res, sent, sent ? ‘Masina incuiata’ : ‘Comanda in asteptare’);
});

// POST /api/locks/unlock
app.post(’/api/locks/unlock’, (req, res) => {
const sent = sendToESP32({ action: ‘unlock’ });
if (sent) carState.locked = false;
apiResponse(res, sent, sent ? ‘Masina descuiata’ : ‘Comanda in asteptare’);
});

// POST /api/climate
app.post(’/api/climate’, (req, res) => {
const { on, temp, fan } = req.body;
if (temp && (temp < 16 || temp > 30)) {
return res.status(400).json({ status: ‘error’, message: ‘Temperatura invalida (16-30)’ });
}
const payload = {
action: ‘climate’,
on:     on   ?? carState.ac_on,
temp:   temp ?? carState.ac_temp,
fan:    fan  ?? carState.ac_fan,
};
const sent = sendToESP32(payload);
if (sent) { carState.ac_on = payload.on; carState.ac_temp = payload.temp; carState.ac_fan = payload.fan; }
apiResponse(res, sent, sent ? (payload.on ? ‘Climatizare pornita’ : ‘Climatizare oprita’) : ‘Comanda in asteptare’);
});

// POST /api/learn
app.post(’/api/learn’, (req, res) => {
const { action } = req.body || {};
if (!action) return res.status(400).json({ status: ‘error’, message: ‘Action lipsa’ });
const sent = sendToESP32({ action: ‘learn_’ + action });
const hints = { lock: ‘Apasa LOCK pe cheie in 5 secunde!’, unlock: ‘Apasa UNLOCK pe cheie in 5 secunde!’, engine_start: ‘Porneste motorul in 5 secunde!’, engine_stop: ‘Opreste motorul in 5 secunde!’, climate: ‘Regleaza clima in 5 secunde!’ };
res.json({ status: ‘ok’, message: hints[action] || ‘Fa actiunea in 5 secunde!’, delivered: sent });
});

// GET /api/sniff – ultimele mesaje CAN
app.get(’/api/sniff’, (req, res) => {
const sent = sendToESP32({ action: ‘sniff’ });
res.json({ status: ‘ok’, requested: sent });
});

// ─── Health check ─────────────────────────────────────────────
app.get(’/’, (req, res) => {
res.json({ service: ‘Mercedes W213 Cloud Bridge’, status: ‘running’, esp32: carState.connected ? ‘online’ : ‘offline’ });
});

// ─── Start ────────────────────────────────────────────────────
const PORT = process.env.PORT || 3000;
server.listen(PORT, () => {
console.log(`\n=== Mercedes W213 Cloud Bridge ===`);
console.log(`Server pornit pe portul ${PORT}`);
console.log(`REST API:  http://localhost:${PORT}/api/status`);
console.log(`WebSocket: ws://localhost:${PORT}`);
console.log(`==================================\n`);
});