# ttyd-nodejs

A Node.js shared PTY terminal with WebSocket and REST API (ttyd-like).

## Features
- Persistent PTY shell session (bash/sh)
- WebSocket server for multiple clients (browser terminal)
- REST API to send commands and get output/exit code
- Shared session for all clients
- Simple HTML/JS client (xterm.js)
- Dockerfile and docker-compose for easy deployment

## Usage

### Local
```bash
npm install
npm start
# Open http://localhost:3000 in your browser
```

### Docker Compose
```bash
docker-compose up --build
# Open http://localhost:3000
```

## REST API

### POST `/api/command`
Send a shell command to the shared PTY session and get the output and exit code.

**Request:**
- Method: `POST`
- URL: `/api/command`
- Content-Type: `application/json`
- Body:
  ```json
  { "command": "ls -l /" }
  ```

**Response:**
- Status: `200 OK`
- Content-Type: `application/json`
- Body:
  ```json
  {
    "stdout": "<command output>\n",
    "exit_code": 0
  }
  ```

**Example:**
```bash
curl -X POST http://localhost:3000/api/command \
  -H 'Content-Type: application/json' \
  -d '{"command": "ls -l /"}'
```

## WebSocket API

- URL: `ws://localhost:3000/ws`
- Protocol: JSON messages

### Messages from Client
- **Input:**
  ```json
  { "type": "input", "data": "ls\n" }
  ```
  Sends keystrokes or commands to the PTY.

- **Resize:**
  ```json
  { "type": "resize", "cols": 80, "rows": 24 }
  ```
  Resizes the PTY session.

- **Signal:**
  ```json
  { "type": "signal", "signal": "SIGINT" }
  ```
  Sends a signal (e.g., Ctrl+C) to the PTY process.

### Messages from Server
- **Output:**
  ```json
  { "type": "output", "data": "<pty output>" }
  ```
  Real-time output from the PTY.

- **Buffer:**
  ```json
  { "type": "buffer", "data": "<last N lines>" }
  ```
  Sent to new clients with the current session buffer.

## WebSocket Terminal
- Connects to ws://localhost:3000/ws
- Sends keystrokes/commands to PTY
- Receives real-time output
- Supports resize and Ctrl+C

## Security
- (Optional) Add authentication as needed

## License
MIT