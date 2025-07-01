// Main server for shared PTY terminal (ttyd-like)
const express = require('express');
const http = require('http');
const { Server: SocketIOServer } = require('socket.io');
const pty = require('node-pty');
const path = require('path');
const serveStatic = require('serve-static');

const START_CMD = process.env.START_CMD || 'tmux new-session -A -s shared';
const PORT = process.env.PORT || 3000;
const WS_PATH = '/ws';
const BUFFER_LINES = 200;

const app = express();
const server = http.createServer(app);
const io = new SocketIOServer(server, { path: WS_PATH });

// Persistent PTY session
// Parse command and args
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

// Broadcast PTY output to all clients
function broadcast(data) {
  io.emit('output', data);
}

ptyProcess.on('data', (data) => {
  appendToBuffer(data);
  broadcast(data);
});

// socket.io: handle input, resize, signals
io.on('connection', (socket) => {
  // Send buffer to new client
  socket.emit('buffer', buffer.join('\n'));

  // Handle input event
  socket.on('input', (data, callback) => {
    ptyProcess.write(data);
    if (callback) callback({ status: 'ok' });
  });

  // Handle resize event
  socket.on('resize', ({ cols, rows }, callback) => {
    ptyProcess.resize(cols, rows);
    if (callback) callback({ status: 'ok' });
  });

  // Handle signal event
  socket.on('signal', (signal, callback) => {
    if (signal === 'SIGINT') {
      ptyProcess.write('\x03'); // Send ASCII Ctrl+C
      if (callback) callback({ status: 'ok' });
    } else {
      if (callback) callback({ status: 'ignored', message: 'Only SIGINT (Ctrl+C) is supported as character send.' });
    }
  });

  // Handle 'cmd' event for running a command and returning clean output
  socket.on('cmd', async (command, callback) => {
    if (!command || typeof command !== 'string') {
      if (callback) return callback({ error: 'Missing command' });
    }
    // Use unique markers to delimit output
    const startMarker = `__CMD_START_${Date.now()}_${Math.random().toString(36).slice(2)}__`;
    const endMarker = `__CMD_END_${Date.now()}_${Math.random().toString(36).slice(2)}__`;
    const wrappedCmd = `echo ${startMarker}; ${command}; echo ${endMarker}$?\n`;
    let output = '';
    let done = false;
    function onData(data) {
      output += data;
      if (output.includes(endMarker)) {
        
        done = true;
      }
    }
    ptyProcess.on('data', onData);
    ptyProcess.write(wrappedCmd);
    // Wait for end marker or timeout
    const timeout = 10 * 1000;
    const start = Date.now();
    while (!done && Date.now() - start < timeout) {
      await new Promise((r) => setTimeout(r, 50));
    }
    ptyProcess.removeListener('data', onData);
    // Extract output between markers
    let stdout = '';
    let exitCode = null;
    const startIdx = output.indexOf(startMarker);
    const endIdx = output.indexOf(endMarker);
    if (startIdx !== -1 && endIdx !== -1) {
      stdout = output.slice(startIdx + startMarker.length, endIdx);
      const afterEnd = output.slice(endIdx + endMarker.length, endIdx + endMarker.length + 4);
      const codeMatch = afterEnd.match(/(\d+)/);
      if (codeMatch) {
        exitCode = parseInt(codeMatch[1], 10);
      }
    } else {
      stdout = output;
    }
    stdout = stdout.replace(/\r/g, '');
    stdout = stdout.replace(/\u001b\[[0-9;]*[a-zA-Z]/g, '');
    stdout = stdout.trim();
    if (callback) callback({ stdout, exit_code: exitCode });
  });
});


function runCommandWithInactivityTimeout(
  ptyProcess,
  command,
  callback,
  timeoutMs = 2000
) {
  let outputBuffer = "";
  let inactivityTimer;

  const resetInactivityTimer = () => {
    clearTimeout(inactivityTimer);
    inactivityTimer = setTimeout(() => {
      finish();
    }, timeoutMs);
  };

  const onData = (data) => {
    outputBuffer += data;
    resetInactivityTimer();
  };

  const finish = () => {
    ptyProcess.removeListener("data", onData);

    const cleaned = outputBuffer
      .replace(/\x1B\[[0-9;]*[a-zA-Z]/g, "") // remove ANSI
      .replace(/\r/g, "")
      .replace(/[\b]/g, "")
      .split("\n")
      .map((line) => line.trim())
      .filter((line) => line.length > 0)
      .filter((line) => line !== command) // remove echoed command
      .filter((line) => !promptPattern.test(line)) // remove prompt lines
      .join("\n");

    callback(null, cleaned);
  };

  // Start listening
  ptyProcess.on("data", onData);

  // Send command
  ptyProcess.write(`${command}\n`);

  // Start first timer
  resetInactivityTimer();
}


// REST API: POST /api/command

app.use(express.json());
app.post('/api/command', async (req, res) => {
  const { command } = req.body;
  if (!command || typeof command !== "string") {
    return res.status(400).json({ error: "Missing command" });
  }

  let outputBuffer = "";
  let responded = false;
  let inactivityTimer;

  function resetInactivityTimer() {
    clearTimeout(inactivityTimer);
    inactivityTimer = setTimeout(finish, 2000); // 2s inactivity
  }

  function onData(data) {
    outputBuffer += data;
    resetInactivityTimer();
  }

  function finish() {
    if (responded) return;
    responded = true;
    clearTimeout(inactivityTimer);
    clearTimeout(hardTimeout);
    ptyProcess.removeListener("data", onData);

    // Clean output
    // Improved prompt pattern: matches common shell prompts at line end, including user@host:path$ or #, with optional spaces
    const promptPattern = /^\s*[\w.@~☁:/\-\[\]{}()=\\$%# ]+[#$%]\s*$/m;
    let cleaned = outputBuffer
      .replace(/\x1B\[[0-9;]*[a-zA-Z]/g, "") // remove ANSI
      .replace(/\r/g, "")
      .replace(/[\b]/g, "")
      .split("\n")
      .map(line => line.trim())
      .filter(line => line.length > 0)
      .filter(line => line !== command) // remove echoed command
      .filter(line => !promptPattern.test(line)) // remove prompt lines
      .join("\n");

    res.json({ output: cleaned });
  }

  // Hard timeout fallback
  const hardTimeout = setTimeout(() => {
    if (!responded) {
      responded = true;
      ptyProcess.removeListener("data", onData);
      res.status(504).json({ error: "Command timed out" });
    }
  }, 10000);

  ptyProcess.on("data", onData);
  ptyProcess.write(`${command}\n`);
  resetInactivityTimer();
});

// Serve static files (client)
app.use(serveStatic(path.join(__dirname, 'public')));

server.listen(PORT, () => {
  console.log(`Server running on http://localhost:${PORT}`);
  console.log(`WebSocket at ws://localhost:${PORT}${WS_PATH}`);
});