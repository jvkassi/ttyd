# PTY HTTP API Server

This project provides a simple HTTP JSON API to interact with PTY (pseudo-terminal) sessions. It allows you to create terminal sessions, send commands, retrieve output, and close sessions remotely.

## Features

*   Create new PTY sessions (defaults to `bash`).
*   Send commands/input to an active session.
*   Retrieve output from a session (non-blocking).
*   Close PTY sessions.

## Requirements

*   Python 3.7+
*   `Flask`
*   `ptyprocess`

## Setup and Running

1.  **Clone the repository (or create the files as provided).**

2.  **Navigate to the project directory:**
    ```bash
    cd pty_http_api
    ```

3.  **Create and activate a Python virtual environment:**
    ```bash
    python3 -m venv venv
    source venv/bin/activate
    ```
    *(On Windows, use `venv\Scripts\activate`)*

4.  **Install dependencies:**
    ```bash
    pip install -r requirements.txt
    ```

5.  **Run the Flask application:**
    ```bash
    python app.py
    ```
    The server will start by default on `http://localhost:5000`.

## API Endpoints

The API uses JSON for requests and responses.

### 1. Create a new PTY session

*   **Endpoint:** `POST /sessions`
*   **Request Body:** None
*   **Success Response (201 Created):**
    ```json
    {
      "session_id": "your-new-session-uuid",
      "message": "Session created successfully"
    }
    ```
*   **Example using `curl`:**
    ```bash
    curl -X POST http://localhost:5000/sessions
    ```

### 2. Send input to a session

*   **Endpoint:** `POST /sessions/<session_id>/input`
*   **URL Parameters:**
    *   `session_id`: The ID of the target session.
*   **Request Body (JSON):**
    ```json
    {
      "command": "your command string here\n"
    }
    ```
    *(Note: Ensure your command includes a newline character `\n` if you expect it to be executed immediately in a shell, just like pressing Enter.)*
*   **Success Response (200 OK):**
    ```json
    {
      "message": "Input sent successfully"
    }
    ```
*   **Error Responses:**
    *   `400 Bad Request`: If "command" field is missing.
    *   `404 Not Found`: If `session_id` is invalid.
    *   `410 Gone`: If the session process is no longer alive.
*   **Example using `curl`:**
    ```bash
    SESSION_ID="your-session-uuid" # Replace with actual ID
    curl -X POST -H "Content-Type: application/json" \
         -d '{"command": "ls -la\\n"}' \
         http://localhost:5000/sessions/$SESSION_ID/input
    ```

### 3. Get output from a session

*   **Endpoint:** `GET /sessions/<session_id>/output`
*   **URL Parameters:**
    *   `session_id`: The ID of the target session.
*   **Success Response (200 OK):**
    The `output` field contains a chunk of text output from the PTY. This is a non-blocking read, so multiple calls might be needed to get all output from a long-running command.
    ```json
    {
      "output": "text output from the PTY session...\n"
    }
    ```
*   **Error Responses:**
    *   `404 Not Found`: If `session_id` is invalid.
    *   `410 Gone`: If the session process is no longer alive.
*   **Example using `curl`:**
    ```bash
    SESSION_ID="your-session-uuid" # Replace with actual ID
    curl http://localhost:5000/sessions/$SESSION_ID/output
    ```

### 4. Close a session

*   **Endpoint:** `DELETE /sessions/<session_id>`
*   **URL Parameters:**
    *   `session_id`: The ID of the session to close.
*   **Success Response (200 OK):**
    ```json
    {
      "message": "Session <session_id> closed successfully"
    }
    ```
*   **Error Responses:**
    *   `404 Not Found`: If `session_id` is invalid.
*   **Example using `curl`:**
    ```bash
    SESSION_ID="your-session-uuid" # Replace with actual ID
    curl -X DELETE http://localhost:5000/sessions/$SESSION_ID
    ```

## Notes

*   The PTY sessions are started with `bash` by default. This can be changed in `app.py`.
*   Output is read in non-blocking chunks (currently up to 4096 bytes per call to `/output`).
*   Sessions are stored in memory and will be lost if the Flask server restarts.
*   This is a basic implementation. For production use, consider aspects like authentication, resource limits, more robust error handling, and potentially an asynchronous framework for better scalability.
