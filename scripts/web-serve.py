#!/usr/bin/env python3
import sys
import os
import http.server

def main():
    if len(sys.argv) < 2:
        print("Usage: web-serve.py <directory>", file=sys.stderr)
        sys.exit(1)

    directory = sys.argv[1]
    os.chdir(directory)

    port = 8000
    while True:
        try:
            server = http.server.HTTPServer(('', port), http.server.SimpleHTTPRequestHandler)
            print(f"Serving HTTP on 0.0.0.0 port {port} (http://127.0.0.1:{port}/) ...")
            try:
                server.serve_forever()
            except KeyboardInterrupt:
                print("\nKeyboard interrupt received, exiting.")
            break
        except OSError:
            port += 1

if __name__ == "__main__":
    main()
