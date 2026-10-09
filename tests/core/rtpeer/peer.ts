// A realtime v2 gateway for tests: the server side of the handshake and of the envelope
// protocol, written on the web client's protocol code (@pwa-rt = <pwa>/src/rt).
//
// One port serves everything:
//   GET  /v2/keys             the static key listing
//   WS   /v2/ws               the connection
//   GET  /control/state       what the peer saw (auths, acks, signals, sends, proofs)
//   POST /control/device      { pub }  the device's Ed25519 public key (base64) for proofs
//   POST /control/push        { kind, data }  a live update to every connection
//   POST /control/signal      { t, message, body }  a signal to every connection
//   POST /control/close       { code }  a Close envelope, then the socket closes
//   POST /control/rotate      a new static key with the next id; the old one is gone
//   POST /control/pts         { pts }  moves the update log's head (missed updates to replay)
//
// The access token picks the scenario:
//   "expired…"   the auth answer is auth.proof_required with a nonce; a valid Proof signs in
//   "revoked…"   the auth answer is an error, then Close 4001
//   anything else signs in.
import { createServer, type IncomingMessage, type ServerResponse } from 'node:http';
import { randomBytes } from 'node:crypto';
import { WebSocketServer, type WebSocket } from 'ws';
import { ed25519, x25519 } from '@noble/curves/ed25519.js';
import { Responder, bytesToHex, concat, type Session } from '@pwa-rt/noise';
import { encodeRecords, RecordDecoder } from '@pwa-rt/frames';
import { decode, encode, fromBase64, jsonify, useSchema } from '@pwa-rt/proto';
import { SCHEMA } from '@pwa-rt/schema';

useSchema(SCHEMA);

const utf8 = new TextEncoder();

interface Key {
  id: number;
  priv: Uint8Array;
  pub: Uint8Array;
}

function newKey(id: number): Key {
  const priv = new Uint8Array(randomBytes(32));
  return { id, priv, pub: x25519.getPublicKey(priv) };
}

let key = newKey(7);
let pts = 100;
let nextSeq = 10;
let devicePub: Uint8Array | null = null;
const seenClientIds = new Map<string, { seq: number; id: string; at: number }>();

const state = {
  auths: [] as Record<string, unknown>[],
  proofs: [] as boolean[],
  acks: [] as number[],
  signals: [] as Record<string, unknown>[],
  sends: [] as Record<string, unknown>[],
  requests: [] as string[],
  pings: 0,
  rejectedKeyIds: [] as number[],
};

class Connection {
  private session: Session | null = null;
  private decoder: RecordDecoder | null = null;
  private nonce: Uint8Array | null = null;
  private authed = false;
  private authPts = 0;

  constructor(private ws: WebSocket) {
    ws.on('message', (data: Buffer) => {
      try {
        this.receive(new Uint8Array(data));
      } catch (error) {
        console.error('rtpeer: connection failed:', error);
        ws.close(1002);
      }
    });
    ws.on('close', () => connections.delete(this));
  }

  get open(): boolean {
    return this.authed;
  }

  send(envelope: Record<string, unknown>) {
    if (!this.session) return;
    for (const record of encodeRecords(this.session.send, encode('Envelope', envelope))) {
      this.ws.send(record);
    }
  }

  close(code: number, reason: string) {
    this.send({ close: { code, reason } });
    this.ws.close(1000);
  }

  private receive(data: Uint8Array) {
    if (!this.session) {
      this.handshake(data);
      return;
    }
    const envelope = this.decoder!.record(data);
    if (envelope) this.dispatch(decode('Envelope', envelope));
  }

  // [key id][message 1 carrying Envelope{ req: auth }]
  private handshake(data: Uint8Array) {
    const keyId = data[0];
    if (keyId !== key.id) {
      // An unknown key: the only clear-text answer there is.
      state.rejectedKeyIds.push(keyId);
      this.ws.send(concat(Uint8Array.of(0xff), encode('Close', { code: 4006, reason: 'key' })));
      this.ws.close(1000);
      return;
    }
    const responder = new Responder(key.priv, utf8.encode('eleza-rt-v2/' + keyId));
    const first = decode('Envelope', responder.readMessage1(data.subarray(1)));
    const request = first.req;
    if (!request || request.method !== 'auth' || request.id !== 1) throw new Error('first envelope is not auth');
    const auth = decode('Auth', request.body ?? new Uint8Array(0));
    state.auths.push({ ...auth, key_id: keyId });
    this.authPts = Number(auth.pts ?? 0);
    const token = String(auth.token ?? '');

    let answer: Record<string, unknown>;
    let then: () => void = () => undefined;
    if (token.startsWith('revoked')) {
      answer = { id: 1, ok: false, error: { code: 'auth.session_revoked', title: 'revoked', http_status: 401 } };
      then = () => this.close(4001, 'revoked');
    } else if (token.startsWith('expired')) {
      this.nonce = new Uint8Array(randomBytes(32));
      answer = {
        id: 1,
        ok: false,
        body: encode('Challenge', { nonce: this.nonce }),
        error: { code: 'auth.proof_required', title: 'proof required', http_status: 401 },
      };
    } else {
      answer = { id: 1, ok: true, body: encode('AuthResult', { expires_at: Date.now() + 3600_000 }) };
      then = () => this.welcome();
    }
    this.ws.send(responder.writeMessage2(encode('Envelope', { res: answer })));
    this.session = responder.session!;
    this.decoder = new RecordDecoder(this.session.recv);
    then();
  }

  // Hello, then what the client missed.
  private welcome() {
    this.authed = true;
    const resumed = this.authPts > 0 && this.authPts < pts;
    this.send({
      hello: {
        v: 2,
        conn: 'conn-' + state.auths.length,
        ping_interval_s: 25,
        reconnect_spread_s: 5,
        pts,
        resumed,
        server_time_ms: Date.now(),
      },
    });
    if (!resumed) return;
    const items: Record<string, unknown>[] = [];
    for (let p = this.authPts + 1; p <= pts; p++) {
      const seq = 1000 + p;
      items.push({
        update: {
          pts: p,
          kind: 'message',
          chat: 'c1',
          seq,
          message: message('c1', seq, 'u_sara', 'replayed ' + p),
        },
      });
    }
    // The replay travels as one batch.
    this.send({ batch: { items } });
    this.send({ signal: { t: 'resumed' } });
  }

  private dispatch(envelope: Record<string, any>) {
    if (envelope.batch) {
      for (const item of envelope.batch.items ?? []) this.dispatch(item);
    } else if (envelope.req) {
      this.request(envelope.req);
    } else if (envelope.signal) {
      this.signal(envelope.signal);
    } else if (envelope.ack) {
      state.acks.push(Number(envelope.ack.pts ?? 0));
    } else if (envelope.ping) {
      state.pings++;
      this.send({ pong: { nonce: envelope.ping.nonce ?? 0 } });
    }
  }

  private respond(id: number, message: string, body: Record<string, unknown>) {
    this.send({ res: { id, ok: true, body: encode(message, body) } });
  }

  private refuse(id: number, status: number, code: string, title: string) {
    this.send({ res: { id, ok: false, error: { code, title, http_status: status } } });
  }

  private request(request: Record<string, any>) {
    const id = Number(request.id);
    const method = String(request.method ?? '');
    const body: Uint8Array = request.body ?? new Uint8Array(0);
    state.requests.push(method);
    if (method === 'proof') {
      const proof = decode('Proof', body);
      const text = concat(utf8.encode('eleza/device-proof/v1\0ws_resume\0'), this.nonce ?? new Uint8Array(0));
      let valid = false;
      try {
        valid = !!devicePub && !!this.nonce && ed25519.verify(proof.sig ?? new Uint8Array(0), text, devicePub);
      } catch {
        valid = false;
      }
      state.proofs.push(valid);
      if (!valid) {
        this.refuse(id, 401, 'auth.proof_invalid', 'bad proof');
        this.close(4004, 'auth');
        return;
      }
      this.respond(id, 'AuthResult', { expires_at: Date.now() + 3600_000 });
      this.welcome();
      return;
    }
    if (!this.authed) {
      this.refuse(id, 401, 'auth.required', 'sign in first');
      return;
    }
    switch (method) {
      case 'auth':
        // A fresh token on a live connection.
        state.auths.push({ ...decode('Auth', body), reauth: true });
        this.respond(id, 'AuthResult', { expires_at: Date.now() + 3600_000 });
        break;
      case 'me.get':
        this.respond(id, 'MeGetResponse', {
          id: 'u_me',
          handle: 'me',
          display_name: 'من',
          bio: '',
          is_private: false,
          verified: true,
          followers: 12,
          following: 34,
          posts: 5,
          visibility: 'public',
        });
        break;
      case 'sync.state':
        this.respond(id, 'SyncStateResponse', { pts });
        break;
      case 'sync.difference': {
        const from = Number(decode('SyncDifferenceRequest', body).pts ?? 0);
        const updates: Record<string, unknown>[] = [];
        for (let p = from + 1; p <= pts; p++) {
          updates.push({ pts: p, kind: 'message', chat: 'c1', seq: 1000 + p, message: message('c1', 1000 + p, 'u_sara', 'difference ' + p) });
        }
        this.respond(id, 'SyncDifferenceResponse', { pts, updates, too_long: false });
        break;
      }
      case 'chat.history': {
        const query = decode('ChatHistoryRequest', body);
        const chat = String(query.chat ?? '');
        const limit = Number(query.limit ?? 3) || 3;
        const messages: Record<string, unknown>[] = [];
        for (let seq = limit; seq >= 1; seq--) {
          // "big" asks for an answer that does not fit one record (65535 bytes).
          const text = chat === 'big' ? String(seq).repeat(40_000) : 'پیام ' + seq;
          messages.push(message(chat, seq, seq % 2 ? 'u_sara' : 'u_me', text));
        }
        this.respond(id, 'ChatHistoryResponse', {
          body: {
            chat: {
              chat,
              kind: 'direct',
              state: 'active',
              participants: [
                { ref: 'u_me', user_id: 'u_me', me: true },
                { ref: 'u_sara', user_id: 'u_sara' },
              ],
              last_seq: limit,
              can_send: true,
            },
            messages,
            has_more_before: false,
          },
        });
        break;
      }
      case 'chat.send': {
        const send = decode('ChatSendRequest', body);
        state.sends.push({ ...jsonify<Record<string, unknown>>(send), idempotency_key: request.idempotency_key ?? '' });
        const clientId = String(send.client_message_id ?? '');
        if (String(send.text ?? '') === 'refuse me') {
          this.refuse(id, 403, 'chat.blocked', 'blocked');
          break;
        }
        const known = seenClientIds.get(clientId);
        if (known) {
          this.respond(id, 'ChatSendResponse', { message_id: known.id, seq: known.seq, created_at: known.at, duplicate: true });
          break;
        }
        const seq = nextSeq++;
        const at = Date.now();
        const view = { ...message(String(send.chat), seq, 'u_me', String(send.text ?? '')), client_message_id: clientId, created_at: at };
        if (send.reply_to_seq) Object.assign(view, { reply_to_seq: send.reply_to_seq });
        seenClientIds.set(clientId, { seq, id: String(view.id), at });
        this.respond(id, 'ChatSendResponse', { message_id: view.id, seq, created_at: at });
        // The sender's own echo, like every other device gets it.
        pts++;
        broadcast({ update: { pts, kind: 'message', data: { message: view } } });
        break;
      }
      default:
        this.refuse(id, 404, 'rt.method_unknown', 'no such method');
    }
  }

  private signal(signal: Record<string, any>) {
    const type = String(signal.t ?? '');
    const raw: Uint8Array = signal.body ?? new Uint8Array(0);
    const id = Number(signal.id ?? 0);
    const names: Record<string, string> = {
      typing: 'TypingSignal',
      state: 'StateSignal',
      presence_sub: 'PresenceSub',
      presence_unsub: 'PresenceSub',
      profile_sub: 'ProfileSub',
      topics_sub: 'TopicsSub',
      feed_sub: 'FeedSub',
      live_location: 'LiveLocationSignal',
    };
    const body = names[type] ? decode(names[type], raw) : {};
    state.signals.push({ t: type, id, body: jsonify(body) });
    if (type === 'typing') {
      // The capability names the chat; everybody in it hears the activity.
      this.send({
        signal: {
          t: 'typing',
          body: encode('TypingSignal', { chat: String(body.cap ?? '').replace(/^cap-/, ''), kind: body.kind, ttl_s: 6 }),
        },
      });
    } else if (type === 'presence_sub') {
      const users: string[] = body.users ?? [];
      if (id) {
        this.respond(id, 'PresenceSubResult', {
          presence: users.map((user) => ({ user, status: 'recently', last_seen_at: 1800000000 })),
          ttl_s: 300,
        });
      }
      for (const user of users) {
        this.send({ signal: { t: 'presence', body: encode('PresenceSignal', { user, status: 'online' }) } });
      }
    }
  }
}

function message(chat: string, seq: number, sender: string, text: string): Record<string, unknown> {
  return { chat, seq, id: 'm_' + chat + '_' + seq, sender, kind: 'text', text, created_at: 1800000000000 + seq };
}

const connections = new Set<Connection>();

function broadcast(envelope: Record<string, unknown>) {
  for (const connection of connections) {
    if (connection.open) connection.send(envelope);
  }
}

// ---- http

function readJson(request: IncomingMessage): Promise<Record<string, any>> {
  return new Promise((resolve) => {
    const chunks: Buffer[] = [];
    request.on('data', (chunk: Buffer) => chunks.push(chunk));
    request.on('end', () => {
      try {
        resolve(chunks.length ? JSON.parse(Buffer.concat(chunks).toString('utf8')) : {});
      } catch {
        resolve({});
      }
    });
  });
}

function reply(response: ServerResponse, status: number, body: unknown) {
  const text = JSON.stringify(body);
  response.writeHead(status, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(text) });
  response.end(text);
}

const http = createServer(async (request, response) => {
  const url = (request.url ?? '').split('?')[0];
  if (request.method === 'GET' && url === '/v2/keys') {
    reply(response, 200, { keys: [{ id: key.id, pub: bytesToHex(key.pub) }], preferred: key.id });
  } else if (request.method === 'GET' && url === '/control/state') {
    reply(response, 200, { ...state, pts, key_id: key.id, connections: [...connections].filter((c) => c.open).length });
  } else if (request.method === 'POST' && url === '/control/device') {
    devicePub = fromBase64(String((await readJson(request)).pub ?? ''));
    reply(response, 200, {});
  } else if (request.method === 'POST' && url === '/control/push') {
    const body = await readJson(request);
    pts++;
    broadcast({ update: { pts, kind: body.kind, data: body.data } });
    reply(response, 200, { pts });
  } else if (request.method === 'POST' && url === '/control/signal') {
    const body = await readJson(request);
    broadcast({ signal: { t: body.t, body: body.message ? encode(body.message, body.body ?? {}) : undefined } });
    reply(response, 200, {});
  } else if (request.method === 'POST' && url === '/control/close') {
    const body = await readJson(request);
    for (const connection of [...connections]) connection.close(Number(body.code ?? 4000), 'test');
    reply(response, 200, {});
  } else if (request.method === 'POST' && url === '/control/rotate') {
    key = newKey(key.id + 1);
    for (const connection of [...connections]) connection.close(4006, 'key');
    reply(response, 200, { id: key.id });
  } else if (request.method === 'POST' && url === '/control/pts') {
    pts = Number((await readJson(request)).pts ?? pts);
    reply(response, 200, { pts });
  } else {
    reply(response, 404, { code: 'not_found', title: 'not found' });
  }
});

const sockets = new WebSocketServer({ noServer: true });
http.on('upgrade', (request, socket, head) => {
  if ((request.url ?? '').split('?')[0] !== '/v2/ws') {
    socket.destroy();
    return;
  }
  sockets.handleUpgrade(request, socket, head, (ws) => {
    connections.add(new Connection(ws));
  });
});

http.listen(Number(process.env.ELEZA_RTPEER_PORT ?? 0), '127.0.0.1', () => {
  const address = http.address();
  const port = typeof address === 'object' && address ? address.port : 0;
  // The test reads this line.
  console.log('READY ' + port);
});

// The peer lives as long as the test that started it holds its stdin.
process.stdin.on('end', () => process.exit(0));
process.stdin.resume();
