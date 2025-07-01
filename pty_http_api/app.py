import uuid
import ptyprocess
import os
import fcntl
import select
from flask import Flask, request, jsonify

app = Flask(__name__)

# Dictionary to store active PTY sessions
# Key: session_id (str)
# Value: dictionary {'process': PTYProcess object, 'output_buffer': byte string}
sessions = {}

# --- Helper function to make PTY output non-blocking ---
def set_non_blocking(fd):
    """Set the file descriptor to non-blocking mode."""
    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)

# --- API Endpoints ---

@app.route('/sessions', methods=['POST'])
def create_session():
    """
    Creates a new PTY session.
    By default, it starts a 'bash' shell.
    """
    session_id = str(uuid.uuid4())
    try:
        # Start a new PTY process (e.g., with bash)
        # You can customize the command and environment variables as needed
        process = ptyprocess.PtyProcess.spawn(['bash'])

        # Set the PTY's master file descriptor to non-blocking for output reading
        set_non_blocking(process.fd)

        sessions[session_id] = {
            'process': process,
            'output_buffer': b'' # Store output as bytes
        }
        app.logger.info(f"Created session: {session_id}")
        return jsonify({'session_id': session_id, 'message': 'Session created successfully'}), 201
    except Exception as e:
        app.logger.error(f"Error creating session: {e}")
        return jsonify({'error': str(e)}), 500


@app.route('/sessions/<session_id>/input', methods=['POST'])
def send_input(session_id):
    """
    Sends input (commands) to a specific PTY session.
    Expects JSON: {"command": "your command\\n"}
    """
    if session_id not in sessions:
        return jsonify({'error': 'Session not found'}), 404

    data = request.get_json()
    if not data or 'command' not in data:
        return jsonify({'error': 'Invalid input. "command" field is required.'}), 400

    command = data['command']

    # Ensure the command is bytes and ends with a newline if it's typical shell interaction
    if not command.endswith('\n'):
        command += '\n'

    command_bytes = command.encode('utf-8')

    try:
        process_info = sessions[session_id]
        process = process_info['process']

        if not process.isalive():
            # Clean up dead process
            del sessions[session_id]
            return jsonify({'error': 'Session process is not alive.'}), 410 # 410 Gone

        process.write(command_bytes)
        # Note: ptyprocess.write() attempts to write all bytes but might not block.
        # For interactive shells, the effect of the command will be seen in subsequent output reads.
        app.logger.info(f"Input sent to session {session_id}: {command.strip()}")
        return jsonify({'message': 'Input sent successfully'}), 200
    except Exception as e:
        app.logger.error(f"Error sending input to session {session_id}: {e}")
        # Check if process is still alive before potentially removing it
        if session_id in sessions and not sessions[session_id]['process'].isalive():
            del sessions[session_id]
            app.logger.info(f"Cleaned up dead session {session_id} after input error.")
        return jsonify({'error': str(e)}), 500


@app.route('/sessions/<session_id>/output', methods=['GET'])
def get_output(session_id):
    """
    Retrieves pending output from a specific PTY session.
    This is a non-blocking read.
    """
    if session_id not in sessions:
        return jsonify({'error': 'Session not found'}), 404

    process_info = sessions[session_id]
    process = process_info['process']

    if not process.isalive():
        # Clean up dead process
        del sessions[session_id]
        return jsonify({'output': '', 'error': 'Session process is not alive.'}), 410 # 410 Gone

    try:
        # Non-blocking read from the PTY
        # The `select` module helps check if there's data to be read without blocking.
        # We use a short timeout for select to make the endpoint responsive.
        r, _, _ = select.select([process.fd], [], [], 0.01) # 10ms timeout

        output_data = b""
        if process.fd in r: # Check if our file descriptor is ready for reading
            try:
                # Read up to a certain number of bytes to avoid very large responses
                # and to ensure the read is non-blocking.
                # ptyprocess.read() can sometimes block if not careful,
                # hence the os.O_NONBLOCK flag on the fd.
                output_data = os.read(process.fd, 4096)
            except BlockingIOError:
                # This is expected if no data is available on a non-blocking fd
                pass
            except Exception as e:
                # Handle other potential read errors
                app.logger.error(f"Error reading output from session {session_id}: {e}")
                # If process died during read attempt, clean up
                if not process.isalive():
                    del sessions[session_id]
                    return jsonify({'output': '', 'error': f'Session died during read: {str(e)}'}), 410
                return jsonify({'error': f'Error reading output: {str(e)}'}), 500

        # Append to our session's output buffer (optional, if you want to accumulate)
        # sessions[session_id]['output_buffer'] += output_data
        # For this API, we'll just return the current chunk.

        # Decode bytes to string (assuming UTF-8, adjust if necessary)
        output_str = output_data.decode('utf-8', errors='replace')

        app.logger.debug(f"Output read from session {session_id}: {len(output_str)} bytes")
        return jsonify({'output': output_str}), 200

    except Exception as e:
        app.logger.error(f"Error processing get_output for session {session_id}: {e}")
        if session_id in sessions and not sessions[session_id]['process'].isalive():
            del sessions[session_id]
            app.logger.info(f"Cleaned up dead session {session_id} after output error.")
        return jsonify({'error': str(e)}), 500


@app.route('/sessions/<session_id>', methods=['DELETE'])
def close_session(session_id):
    """
    Closes a specific PTY session.
    """
    if session_id not in sessions:
        return jsonify({'error': 'Session not found'}), 404

    process_info = sessions[session_id]
    process = process_info['process']

    try:
        if process.isalive():
            # Terminate the process gracefully first
            process.terminate(force=False)
            # You might want a short wait here or a more robust check
            # Forcing if it doesn't terminate after a timeout could be added.
            # For simplicity, we'll assume terminate works or rely on subsequent cleanup.
            # process.sendeof() # Another option before terminate
            # process.close() # This closes the PTY master fd, often leads to process exit

        # Ensure it's removed from our tracking
        del sessions[session_id]
        app.logger.info(f"Session {session_id} closed and removed.")
        return jsonify({'message': f'Session {session_id} closed successfully'}), 200
    except Exception as e:
        app.logger.error(f"Error closing session {session_id}: {e}")
        # If it's already gone or an error occurs, try to remove from dict anyway
        if session_id in sessions:
            del sessions[session_id]
        return jsonify({'error': str(e)}), 500


if __name__ == '__main__':
    # It's good practice to make host and port configurable,
    # but for this example, we'll hardcode them.
    # Use 0.0.0.0 to make it accessible from outside the container/VM if needed.
    app.run(host='0.0.0.0', port=5000, debug=True)
