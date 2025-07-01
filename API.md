# ttyd HTTP API

This document describes the HTTP API added to ttyd for executing commands and receiving JSON responses.

## Overview

The ttyd HTTP API allows you to execute shell commands in the active ttyd terminal session and receive the results in JSON format. This enables programmatic interaction with the terminal session without requiring a WebSocket connection.

> **Note:** Commands executed via the HTTP API will appear in the ttyd terminal window and have access to the terminal environment variables and state. The API requires an active terminal session to be open in a browser window.

## API Endpoint

### Execute Command

```
GET /api/{command}
```

Executes the specified command and returns the results as JSON.

#### Parameters

- `{command}`: The shell command to execute, URL-encoded.

#### Example

```
GET /api/ls%20-la
```

#### Response

```json
{
  "stdout": "command output",
  "stderr": "error output if any",
  "exit_code": 0
}
```

## Usage Examples

### Simple Command

```bash
curl "http://localhost:12000/api/ls"
```

### Command with Arguments

```bash
curl "http://localhost:12000/api/ls%20-la"
```

### Command with Special Characters

```bash
curl "http://localhost:12000/api/echo%20%27Hello%20World%21%27"
```

### Command with Pipes (using bash -c)

```bash
curl "http://localhost:12000/api/bash%20-c%20%22ls%20%7C%20grep%20CMake%22"
```

## Error Handling

If the command produces an error, the error output will be included in the `stderr` field, and the `exit_code` will be non-zero:

```json
{
  "stdout": "",
  "stderr": "ls: cannot access '/nonexistent': No such file or directory\n",
  "exit_code": 2
}
```

If no command is provided, the API will return an error:

```json
{
  "error": "No command provided",
  "exit_code": 1
}
```

## Security Considerations

The API executes commands with the same permissions as the ttyd process. Be careful when exposing this API to untrusted users, as it could allow arbitrary command execution on the server.

Consider implementing authentication and authorization mechanisms if you need to expose this API to untrusted users.

## Starting ttyd with the API

To use the API, start ttyd with the `-W` option to allow writing:

```bash
ttyd -p 12000 -W bash
```

This will start ttyd on port 12000 with the bash shell and enable the HTTP API.