// server.js — The Great Cryptid Road Trip
// Multiplayer game server: authoritative state, 60Hz tick, 4-player sync
// Run: node server.js

const express = require('express');
const http = require('http');
const { Server } = require('socket.io');
const path = require('path');

const app = express();
const server = http.createServer(app);
const io = new Server(server, {
  cors: { origin: '*' },
  pingInterval: 500,
  pingTimeout: 2000,
});

// Serve static files for the WASM client
app.use(express.static(path.join(__dirname, 'public')));

// --- Constants ---
const TICK_RATE = 60;
const TICK_INTERVAL = 1000 / TICK_RATE;
const MAX_PLAYERS = 4;
const MOVE_SPEED = 250;    // pixels per second
const JUMP_VEL = -450;     // pixels per second (negative = up)
const GRAVITY = 1200;      // pixels per second squared
const WORLD_W = 1280;
const WORLD_H = 720;
const GROUND_Y = 600;      // floor position

// Cryptid definitions — expandable for RPG classes
const CRYPTIDS = [
  { id: 'bigfoot',      label: 'Bigfoot',      color: '#8B5E3C' },
  { id: 'mothman',      label: 'Mothman',      color: '#A020F0' },
  { id: 'jersey_devil', label: 'Jersey Devil', color: '#DC143C' },
  { id: 'chupacabra',   label: 'Chupacabra',   color: '#2E8B57' },
];

// --- Player State ---
class Player {
  constructor(socketId, cryptidIndex) {
    this.socketId = socketId;
    this.cryptid = CRYPTIDS[cryptidIndex];
    this.x = 200 + cryptidIndex * 200;
    this.y = GROUND_Y - 32;
    this.vx = 0;
    this.vy = 0;
    this.onGround = true;
    // Input buffer (latest received from client)
    this.input = { left: false, right: false, jump: false };
  }
}

const players = new Map();

// --- Input Handling ---
io.on('connection', (socket) => {
  // Find first open cryptid slot
  const occupiedSlots = new Set([...players.values()].map(p => p.cryptid.id));
  const slotIndex = CRYPTIDS.findIndex(c => !occupiedSlots.has(c.id));

  if (slotIndex === -1) {
    socket.emit('full', { message: 'Server is full (4/4 cryptids).' });
    socket.disconnect();
    return;
  }

  const player = new Player(socket.id, slotIndex);
  players.set(socket.id, player);

  // Send player their own identity
  socket.emit('assigned', {
    socketId: socket.id,
    cryptidIndex: slotIndex,
    cryptid: player.cryptid,
  });

  // Tell everyone about the new player
  socket.broadcast.emit('player_joined', {
    socketId: socket.id,
    cryptidIndex: slotIndex,
    x: player.x,
    y: player.y,
  });

  // Send existing players to the newcomer
  for (const [id, p] of players) {
    if (id !== socket.id) {
      socket.emit('player_joined', {
        socketId: id,
        cryptidIndex: CRYPTIDS.findIndex(c => c.id === p.cryptid.id),
        x: p.x,
        y: p.y,
      });
    }
  }

  // Receive client input
  socket.on('input', (data) => {
    const p = players.get(socket.id);
    if (p) {
      p.input.left = !!data.left;
      p.input.right = !!data.right;
      p.input.jump = !!data.jump;
    }
  });

  // Handle disconnect
  socket.on('disconnect', () => {
    players.delete(socket.id);
    io.emit('player_left', { socketId: socket.id });
  });
});

// --- Game Tick (60Hz) ---
function gameTick() {
  for (const [id, p] of players) {
    // Horizontal movement
    p.vx = 0;
    if (p.input.left)  p.vx = -MOVE_SPEED;
    if (p.input.right) p.vx =  MOVE_SPEED;

    // Jump
    if (p.input.jump && p.onGround) {
      p.vy = JUMP_VEL;
      p.onGround = false;
    }

    // Apply gravity
    p.vy += GRAVITY * (TICK_INTERVAL / 1000);

    // Integrate position
    p.x += p.vx * (TICK_INTERVAL / 1000);
    p.y += p.vy * (TICK_INTERVAL / 1000);

    // World bounds
    if (p.x < 0) p.x = 0;
    if (p.x > WORLD_W - 32) p.x = WORLD_W - 32;

    // Ground collision
    if (p.y >= GROUND_Y - 32) {
      p.y = GROUND_Y - 32;
      p.vy = 0;
      p.onGround = true;
    }
  }

  // Broadcast authoritative state to all clients
  const state = {};
  for (const [id, p] of players) {
    state[id] = {
      x: p.x,
      y: p.y,
    };
  }
  io.emit('state', state);
}

setInterval(gameTick, TICK_INTERVAL);

// --- Start ---
const PORT = process.env.PORT || 3000;
server.listen(PORT, () => {
  console.log(`[Cryptid Server] Listening on port ${PORT}`);
  console.log(`[Cryptid Server] Tick rate: ${TICK_RATE}Hz`);
  console.log(`[Cryptid Server] Open http://localhost:${PORT}`);
});
