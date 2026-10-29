import binascii
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib


def chunk(kind, payload):
    return (struct.pack('>I', len(payload)) + kind + payload
            + struct.pack('>I', binascii.crc32(kind + payload) & 0xffffffff))


IMAGE = (b'\x89PNG\r\n\x1a\n'
         + chunk(b'IHDR', struct.pack('>IIBBBBB', 16, 10, 8, 2, 0, 0, 0))
         + chunk(b'IDAT', zlib.compress((b'\0' + bytes((80, 140, 200)) * 16) * 10))
         + chunk(b'IEND', b''))


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def do_GET(self):
        try:
            if self.path in ('/redirect', '/insecure-redirect', '/redirect-loop', '/untrusted'):
                target = {'/redirect': 'https://artwork.invalid/valid.png',
                          '/insecure-redirect': 'http://artwork.invalid/valid.png',
                          '/redirect-loop': 'https://artwork.invalid/redirect-loop',
                          '/untrusted': 'https://wrong-name.invalid/valid.png'}[self.path]
                self.send_response(302)
                self.send_header('Location', target)
                self.send_header('Content-Length', '0')
                self.end_headers()
                return
            if self.path == '/missing':
                self.send_response(404)
                self.send_header('Content-Length', '0')
                self.end_headers()
                return
            self.send_response(200)
            if self.path == '/oversized-header':
                self.send_header('Content-Length', str(1024 * 1024 + 1))
                self.end_headers()
                return
            if self.path in ('/oversized-stream', '/stall'):
                self.send_header('Connection', 'close')
                self.end_headers()
                if self.path == '/stall':
                    time.sleep(3)
                    return
                self.wfile.write(b'x' * (1024 * 1024 + 1))
                return
            body = b'not an image' if self.path == '/invalid.png' else IMAGE
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass


def main():
    if not Path('/.dockerenv').exists():
        raise SystemExit('Run this fixture only inside a disposable manager test container')
    with tempfile.TemporaryDirectory(prefix='thumbnail-https-') as temporary:
        root = Path(temporary)
        key, certificate = root / 'key.pem', root / 'cert.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                        '-keyout', str(key), '-out', str(certificate), '-days', '1',
                        '-subj', '/CN=artwork.invalid', '-addext', 'subjectAltName=DNS:artwork.invalid'],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with Path('/etc/ssl/certs/ca-certificates.crt').open('ab') as trust:
            trust.write(certificate.read_bytes())
        server = ThreadingHTTPServer(('127.0.0.1', 443), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            subprocess.run(sys.argv[1:], check=True, timeout=30)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == '__main__':
    main()
