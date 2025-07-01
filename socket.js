const { Server: SocketIOServer } = require('socket.io');

module.exports = function(server, WS_PATH, ptyProcess, buffer, appendToBuffer) {
  const io = new SocketIOServer(server, { path: WS_PATH });

  function broadcast(data) {
    io.emit('output', data);
  }

  ptyProcess.on('data', (data) => {
    appendToBuffer(data);
    broadcast(data);
  });

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
};