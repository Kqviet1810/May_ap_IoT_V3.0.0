#!/usr/bin/env python3
"""Offline integration against real workerd/SQLite DO, never remote deployment.
Seeds a temporary local D1 fixture; production Google login is not bypassed.
"""
import argparse
import base64
import hashlib
import hmac
import json
import os
from pathlib import Path
import secrets
import select
import socket
import struct
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
PEPPER = 'local-realtime-test-only'
DEVICE = 'MAP-ABCDEF123456'
KEY = '77' * 32
SESSION = '11' * 24
TOKEN = '22' * 32


def digest(value):
    return hashlib.sha256((PEPPER + ':' + value).encode()).hexdigest()


class WebSocket:
    def __init__(self, port, path, headers):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=5)
        self.sock.settimeout(5)
        self.buffer = bytearray()
        nonce = base64.b64encode(secrets.token_bytes(16)).decode()
        request = f'GET {path} HTTP/1.1\r\nHost: localhost:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {nonce}\r\nSec-WebSocket-Version: 13\r\n'
        request += ''.join(f'{key}: {value}\r\n' for key, value in headers.items()) + '\r\n'
        self.sock.sendall(request.encode())
        while b'\r\n\r\n' not in self.buffer:
            self.buffer.extend(self.sock.recv(4096))
        header, rest = bytes(self.buffer).split(b'\r\n\r\n', 1)
        self.buffer = bytearray(rest)
        self.status = int(header.split(b' ')[1])
        if self.status == 101:
            expected = base64.b64encode(hashlib.sha1((nonce + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
            fields = {key.lower(): value.strip() for line in header.split(b'\r\n')[1:] if b':' in line for key, value in [line.split(b':', 1)]}
            assert fields[b'sec-websocket-accept'] == expected

    def take(self, size):
        while len(self.buffer) < size:
            value = self.sock.recv(4096)
            if not value:
                raise EOFError('WebSocket disconnected')
            self.buffer.extend(value)
        out = bytes(self.buffer[:size])
        del self.buffer[:size]
        return out

    def send(self, value, opcode=1):
        payload = value if isinstance(value, bytes) else json.dumps(value, separators=(',', ':')).encode()
        mask = secrets.token_bytes(4)
        header = bytes([128 | opcode, 128 | (len(payload) if len(payload) < 126 else 126)])
        if len(payload) >= 126:
            header += struct.pack('!H', len(payload))
        self.sock.sendall(header + mask + bytes(v ^ mask[i % 4] for i, v in enumerate(payload)))

    def receive(self):
        a, b = self.take(2)
        size = b & 127
        if size == 126:
            size = struct.unpack('!H', self.take(2))[0]
        elif size == 127:
            size = struct.unpack('!Q', self.take(8))[0]
        assert size <= 2048 and not b & 128 and a & 128
        payload = self.take(size)
        if a & 15 == 1:
            value = json.loads(payload)
            if 'deliveryId' in value:
                self.send({'kind': 'received', 'deliveryId': value['deliveryId']})
            return value
        return {'opcode': a & 15, 'payload': payload}

    def until(self, predicate, limit=30):
        for _ in range(limit):
            message = self.receive()
            if predicate(message):
                return message
        raise AssertionError('Expected WebSocket message not received')

    def close(self):
        self.sock.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--browser', action='store_true', help='Also test actual native Chromium transport (Playwright browser required)')
    args = parser.parse_args()
    wrangler = ROOT / 'cloudflare/node_modules/.bin/wrangler'
    assert wrangler.exists(), 'Install pinned cloudflare dependencies first'
    subprocess.run(['python3', str(ROOT / 'tools/build_web_assets.py')], check=True)
    env = {**os.environ, 'WRANGLER_SEND_METRICS': 'false'}
    with tempfile.TemporaryDirectory(prefix='mayap-workerd-') as temporary:
        state = Path(temporary)
        now = int(time.time() * 1000)
        ddl = (ROOT / 'cloudflare/schema.sql').read_text()
        for name in ['0003_telemetry_history', '0004_accounts', '0005_account_picture', '0007_alarm_delivery']:
            ddl += '\n' + (ROOT / f'cloudflare/migrations/{name}.sql').read_text()
        ddl += f"""
INSERT INTO users(google_sub,created_at,last_login_at) VALUES('fixture-owner',{now},{now});
INSERT INTO user_sessions(id,user_sub,token_hash,created_at,expires_at) VALUES('{SESSION}','fixture-owner','{digest(TOKEN)}',{now},{now + 3600000});
INSERT INTO devices(device_id,device_key_hash,created_at) VALUES('{DEVICE}','{digest(KEY)}',{now});
INSERT INTO user_devices(user_sub,device_id,role,created_at) VALUES('fixture-owner','{DEVICE}','owner',{now});
"""
        sql = state / 'fixture.sql'
        sql.write_text(ddl)
        subprocess.run([str(wrangler), 'd1', 'execute', 'mayap_push', '--local', '--persist-to', str(state), '--file', str(sql)], cwd=ROOT / 'cloudflare', env=env, check=True, stdout=subprocess.DEVNULL)
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            port = probe.getsockname()[1]
        with (state / 'worker.log').open('w+') as log:
            process = subprocess.Popen([str(wrangler), 'dev', '--local', '--port', str(port), '--persist-to', str(state),
                '--var', f'MAYAP_SESSION_PEPPER:{PEPPER}', '--var', f'DEVICE_KEY_PEPPER:{PEPPER}',
                '--var', 'ALLOWED_ORIGIN:https://web.test'], cwd=ROOT / 'cloudflare', env=env, stdout=log, stderr=subprocess.STDOUT)
            sockets = []
            try:
                base = f'http://localhost:{port}'
                for _ in range(150):
                    if process.poll() is not None:
                        raise RuntimeError('workerd failed to start')
                    try:
                        with urllib.request.urlopen(base + '/index.html', timeout=1) as response:
                            assert b'realtime_transport.js' in response.read()
                        break
                    except (OSError, urllib.error.URLError):
                        time.sleep(.2)
                else:
                    raise RuntimeError('workerd startup deadline')
                def ws(path, headers):
                    value = WebSocket(port, path, headers)
                    sockets.append(value)
                    return value
                def admission():
                    request = urllib.request.Request(base + '/api/device/realtime-session', data=json.dumps({'device_id': DEVICE, 'control_client_id': 'w-integration123'}).encode(), headers={'Origin':'https://web.test','Authorization':'Bearer '+TOKEN,'Content-Type':'application/json'})
                    with urllib.request.urlopen(request, timeout=5) as response:
                        return json.load(response)
                # Actual D1 atomic receipt and retry after a lost device response.
                def alarm_receipt():
                    body={'device_id':DEVICE,'device_key':KEY,'event_id':'integration-0001',
                          'alarm_type':'FAULT_130','state':'active','severity':'critical','message':'heater switch off'}
                    request=urllib.request.Request(base+'/api/device/alarm',data=json.dumps(body).encode(),
                        headers={'Content-Type':'application/json'})
                    with urllib.request.urlopen(request,timeout=5) as response:return json.load(response)
                receipt=alarm_receipt()
                assert receipt.get('durable') and receipt.get('event_id')=='integration-0001',receipt
                assert alarm_receipt().get('duplicate') is True
                assert ws('/realtime/device/' + DEVICE, {}).status == 401
                d = ws('/realtime/device/' + DEVICE, {'Authorization':'Bearer '+KEY,'X-Mayap-Boot':'123'})
                assert d.status == 101, d.status
                session = admission()
                headers = {'Origin':'https://web.test','Sec-WebSocket-Protocol':'mayap.v1, ticket.'+session['realtime']['ticket']}
                b = ws('/realtime/browser/' + DEVICE, headers)
                assert b.status == 101
                assert b.until(lambda m: m.get('kind') == 'ready')['deviceId'] == DEVICE
                assert ws('/realtime/browser/' + DEVICE, headers).status == 403, 'ticket replay'
                b.send({'kind':'ping'})
                assert b.until(lambda m: m.get('kind') == 'pong')['kind'] == 'pong'
                d.send(b'protocol-heartbeat', 9)
                assert d.until(lambda m: m.get('opcode') == 10)['payload'] == b'protocol-heartbeat'
                b.send({'v':1,'channel':'session','payload':{'clientId':'w-integration123','active':True,'foreground':True,'ttlMs':45000,'sync':True,'scope':'runtime'}})
                assert d.until(lambda m: m.get('channel') == 'session')['payload']['sync'] is True
                d.send({'v':1,'channel':'snapshot','payload':{'bootId':123,'runtime':{'temperature':37.5}}})
                assert b.until(lambda m: m.get('channel') == 'snapshot')['payload']['runtime']['temperature'] == 37.5
                control = session['control']
                body = json.dumps({'v':2,'clientId':'w-integration123','requestId':'cmd-real','seq':1,'nonce':'ab'*8,'bootId':123,'action':'light_toggle','expiresAt':int(time.time())+20}, separators=(',',':'))
                signed = f"mayap-mqtt-write:v2\n{DEVICE}\ncommand\n{control['grant']}\n{body}"
                wire = {'v':2,'body':body,'grant':control['grant'],'grantSig':control['grantSig'],'sig':hmac.new(bytes.fromhex(control['sessionKey']),signed.encode(),hashlib.sha256).hexdigest()}
                command = {'v':1,'channel':'command','payload':wire}
                b.send(command)
                assert d.until(lambda m: m.get('channel') == 'command')['payload'] == wire
                assert b.until(lambda m: m.get('kind') == 'forwarded')['requestId'] == 'cmd-real'
                # Application ACK remains a device message with its original signature.
                d.send({'v':1,'channel':'ack','payload':{'v':2,'bootId':123,'requestId':'cmd-real','sig':'device-signature-fixture'}})
                assert b.until(lambda m: m.get('channel') == 'ack')['payload']['sig'] == 'device-signature-fixture'
                b.send(command)
                assert d.until(lambda m: m.get('channel') == 'command')['payload'] == wire
                # Same tab reconnect: DO SQLite keeps the exact-retry fingerprint.
                reconnect_session = admission()
                b = ws('/realtime/browser/' + DEVICE, {'Origin':'https://web.test', 'Sec-WebSocket-Protocol':'mayap.v1, ticket.'+reconnect_session['realtime']['ticket']})
                assert b.status == 101
                b.until(lambda m: m.get('kind') == 'ready')
                b.send(command)
                assert d.until(lambda m: m.get('channel') == 'command')['payload'] == wire
                b.send({'v':1,'channel':'session','payload':{'clientId':'w-integration123','active':True,'ttlMs':45000}})
                d2 = ws('/realtime/device/'+DEVICE, {'Authorization':'Bearer '+KEY,'X-Mayap-Boot':'124'})
                assert d2.status == 101
                assert d2.until(lambda m: m.get('channel') == 'session')['payload']['active'] is True
                b.send(command)
                assert b.until(lambda m: m.get('kind') == 'error')['code'] == 'INVALID_SIGNATURE_OR_EXPIRY'
                b.send({'v':1,'channel':'session','payload':{'clientId':'w-integration123','active':True,'foreground':False,'ttlMs':45000}})
                assert d2.until(lambda m: m.get('channel') == 'session')['payload']['active'] is False
                b.send(b'x'*2049)
                assert b.until(lambda m: m.get('opcode') == 8)['payload'][:2] == struct.pack('!H',1009)
                if args.browser:
                    subprocess.run(['node', str(ROOT / 'tools/test_realtime_browser.cjs'), str(port)], cwd=ROOT, env=env, check=True, timeout=30)
                new_key = '88' * 32
                rotate = urllib.request.Request(base + '/api/device/rotate-key', data=json.dumps({'device_id':DEVICE,'device_key':KEY,'new_device_key':new_key}).encode(), headers={'Content-Type':'application/json'})
                with urllib.request.urlopen(rotate, timeout=5) as response:
                    assert json.load(response)['success'] is True
                assert d2.until(lambda m: m.get('opcode') == 8)['payload'][:2] == struct.pack('!H',4003)
                assert ws('/realtime/device/'+DEVICE, {'Authorization':'Bearer '+KEY,'X-Mayap-Boot':'124'}).status == 401
                assert ws('/realtime/device/'+DEVICE, {'Authorization':'Bearer '+new_key,'X-Mayap-Boot':'124'}).status == 101
                # Live read revocation is an API guarantee, independent of lease.
                revoke_session = admission()
                live = ws('/realtime/browser/' + DEVICE, {'Origin':'https://web.test', 'Sec-WebSocket-Protocol':'mayap.v1, ticket.'+revoke_session['realtime']['ticket']})
                assert live.status == 101
                logout = urllib.request.Request(base + '/api/account/logout', data=b'{}', headers={'Origin':'https://web.test','Authorization':'Bearer '+TOKEN,'Content-Type':'application/json'})
                with urllib.request.urlopen(logout, timeout=5) as response:
                    assert json.load(response)['success'] is True
                assert live.until(lambda m: m.get('opcode') == 8)['payload'][:2] == struct.pack('!H',4003)
                print('Real workerd: assets, per-device auth, one-use browser admission, auto-ping, snapshot, signed command, exact retry, controller ACK, reconnect/boot fence, hidden lease, frame cap, credential rotation, persistent replay and immediate logout revocation PASS')
            except BaseException:
                log.flush();log.seek(0)
                print(log.read()[-8000:])
                raise
            finally:
                for value in sockets:
                    value.close()
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill();process.wait()


if __name__ == '__main__':
    main()
