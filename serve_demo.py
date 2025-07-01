#!/usr/bin/env python3
import http.server
import socketserver
import urllib.parse
import requests
import json
import sys

# Default port for the demo server
PORT = 12001
# Default ttyd API URL
TTYD_API_URL = "http://localhost:12000/api"

class ProxyHandler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        # Serve the demo HTML file
        if self.path == "/" or self.path == "/index.html":
            self.path = "/api_demo.html"
            return http.server.SimpleHTTPRequestHandler.do_GET(self)
        
        # Proxy API requests to ttyd
        elif self.path.startswith("/api/"):
            # Extract the command from the URL
            command = self.path[5:]  # Remove "/api/"
            if not command:
                self.send_error(400, "No command provided")
                return
            
            # URL decode the command
            command = urllib.parse.unquote(command)
            
            try:
                # Forward the request to ttyd API
                response = requests.get(f"{TTYD_API_URL}/{urllib.parse.quote(command)}")
                
                # Set response headers
                self.send_response(response.status_code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                
                # Send the response body
                self.wfile.write(response.content)
            except Exception as e:
                self.send_error(500, f"Error: {str(e)}")
        else:
            # Serve static files
            return http.server.SimpleHTTPRequestHandler.do_GET(self)

def main():
    global PORT, TTYD_API_URL
    
    # Parse command line arguments
    if len(sys.argv) > 1:
        PORT = int(sys.argv[1])
    if len(sys.argv) > 2:
        TTYD_API_URL = sys.argv[2]
    
    # Create the server
    with socketserver.TCPServer(("0.0.0.0", PORT), ProxyHandler) as httpd:
        print(f"Serving demo at http://localhost:{PORT}")
        print(f"Proxying API requests to {TTYD_API_URL}")
        httpd.serve_forever()

if __name__ == "__main__":
    main()