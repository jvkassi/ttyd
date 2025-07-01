const express = require('express');
const http = require('http');
const path = require('path');
const serveStatic = require('serve-static');
const pty = require('node-pty');

const START_CMD = process.env.START_CMD || 'tmux new-session -A -s shared';
const PORT = process.env.PORT || 3000;
const WS_PATH = '/ws';
const BUFFER_LINES = 200;

const app = express();
const server = http.createServer(app);

// Persistent PTY session
const [ptyCmd, ...ptyArgs] = START_CMD.split(' ');
const ptyProcess = pty.spawn(ptyCmd, ptyArgs, {
  name: 'xterm-color',
  cols: 80,
  rows: 24,
  cwd: process.env.HOME,
  env: process.env,
});

// Buffer for last N lines
let buffer = [];
let bufferString = '';
function appendToBuffer(data) {
  bufferString += data;
  let lines = bufferString.split(/\r?\n/);
  if (lines.length > 1) {
    buffer = buffer.concat(lines.slice(0, -1));
    bufferString = lines[lines.length - 1];
    if (buffer.length > BUFFER_LINES) {
      buffer = buffer.slice(buffer.length - BUFFER_LINES);
    }
  }
}

// Import and set up socket.io
require('./socket')(server, WS_PATH, ptyProcess, buffer, appendToBuffer);
// Import and set up REST API
require('./api')(app, ptyProcess);

// Serve static files (client)
app.use(serveStatic(path.join(__dirname, 'public')));

server.listen(PORT, () => {
  console.log(`Server running on http://localhost:${PORT}`);
  console.log(`WebSocket at ws://localhost:${PORT}${WS_PATH}`);
});