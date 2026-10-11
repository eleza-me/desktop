#!/usr/bin/env node
// Two users who can call each other: the call API of the Eleza backend
// (/v1/calls…) with its call.* signals, in front of two copies of the web
// client's mock API and a real LiveKit server.
//
//   livekit-server --dev --bind 127.0.0.1 &          # ws://127.0.0.1:7880, devkey / secret
//   node tests/tools/mock-calls.mjs                  # A: http://127.0.0.1:8600   B: :8601
//   Eleza --api=http://127.0.0.1:8600 --workdir=/tmp/wd-calls-a --no-single-instance --debug-login=+989121234567:123456
//   Eleza --api=http://127.0.0.1:8601 --workdir=/tmp/wd-calls-b --no-single-instance --debug-login=+989121234568:123456
//
// Each address is one user. Both live in the web mock's small world, where
// the signed-in user is "u_me" and the first chat (c1) is with "u_sara": each
// of the two sees the other one as سارا. A call started in that chat from one
// app rings in the other.
//
// What is served here, per user (everything else goes to that user's copy
// of the web mock):
//
//   /v1/calls…        the routes of internal/voice of the backend, with its
//                     rules: one call per user (409 call.busy), the callee's
//                     privacy (403 call.not_allowed), reject / cancel / hangup
//                     by state (409 call.state), the ring timeout, the grace
//                     for an answered call that never connects, the "delivered"
//                     flag, GET …/incoming and …/active. The join answer names
//                     the LiveKit server and carries a token minted here.
//   /v2/keys, /v2/ws  the realtime connection of that user: a gateway built
//                     from the web client's own protocol code
//                     (tests/core/rtpeer, ELEZA_RTPEER_GATEWAY=1). call.incoming,
//                     call.ringing, call.accepted, call.answered_elsewhere and
//                     call.ended arrive over it, and so does the call record
//                     the chat gets when a call ends. NO_REALTIME=1 leaves it
//                     out: the apps then live on polling, as with a dead socket.
//
// For tests (any of the two addresses):
//
//   GET  /control/calls      every call with its state, and the qos reports
//   POST /control/kick       { "user": "a" | "b" }   removes that user from the
//                            call's room on the SFU (a media drop -> rejoin)
//   POST /control/privacy    { "user": "a" | "b", "calls": "nobody" | "thread_contacts" }
//   POST /control/busy       { "user": "a" | "b", "busy": true | false }   that user talks
//                            to somebody else: a call to them is refused (409 call.busy)
//   POST /control/signals    { "user": "a" | "b", "down": true | false }   closes /
//                            reopens that user's realtime gateway
//
// Environment: PORT (8600; B is PORT + 1), LIVEKIT_URL (ws://127.0.0.1:7880),
// LIVEKIT_KEY / LIVEKIT_SECRET (devkey / secret), RING_TIMEOUT_MS (45000),
// CONNECT_GRACE_MS (20000), PWA_MOCK (the web client's scripts/mock-api.mjs),
// ELEZA_PWA_DIR (the web client's checkout, for the gateway), NO_REALTIME.
import http from 'node:http';
import net from 'node:net';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const PORT = Number(process.env.PORT || 8600);
const LIVEKIT_URL = process.env.LIVEKIT_URL || 'ws://127.0.0.1:7880';
const LIVEKIT_HTTP = LIVEKIT_URL.replace(/^ws/, 'http');
const LIVEKIT_KEY = process.env.LIVEKIT_KEY || 'devkey';
const LIVEKIT_SECRET = process.env.LIVEKIT_SECRET || 'secret';
const RING_TIMEOUT_MS = Number(process.env.RING_TIMEOUT_MS || 45000);
const CONNECT_GRACE_MS = Number(process.env.CONNECT_GRACE_MS || 20000);
const TOKEN_TTL_S = 300;
const NO_REALTIME = process.env.NO_REALTIME === '1';
const PWA_DIR = process.env.ELEZA_PWA_DIR
	|| ['../../../pwa', '../../../../pwa', '../../../../../pwa']
		.map((dir) => path.resolve(HERE, dir))
		.find((dir) => fs.existsSync(path.join(dir, 'scripts/mock-api.mjs')));
const PWA_MOCK = process.env.PWA_MOCK || (PWA_DIR && path.join(PWA_DIR, 'scripts/mock-api.mjs'));
const PEER = 'u_sara'; // how each of the two sees the other

if (!PWA_MOCK || !fs.existsSync(PWA_MOCK)) {
	console.error('mock-calls: the web client\'s scripts/mock-api.mjs was not found (set PWA_MOCK or ELEZA_PWA_DIR)');
	process.exit(77);
}

// ---- the two users

const users = {
	a: { name: 'a', port: PORT, apiPort: PORT + 10, rtPort: 0, call: null, privacy: 'thread_contacts', nextSeq: 1000, rtProcess: null },
	b: { name: 'b', port: PORT + 1, apiPort: PORT + 11, rtPort: 0, call: null, privacy: 'thread_contacts', nextSeq: 1000, rtProcess: null },
};
const other = (user) => (user === users.a ? users.b : users.a);
const children = [];

function startWebMock(user) {
	const child = spawn(process.execPath, [PWA_MOCK], {
		env: { ...process.env, PORT: String(user.apiPort) },
		stdio: ['ignore', 'ignore', 'inherit'],
	});
	children.push(child);
}

// The gateway holds its stdin: it ends with this process.
function startGateway(user) {
	return new Promise((resolve, reject) => {
		const child = spawn(process.execPath, [path.resolve(HERE, '../core/rtpeer/run.mjs')], {
			env: {
				...process.env,
				ELEZA_RTPEER_GATEWAY: '1',
				ELEZA_RTPEER_PORT: '0',
				ELEZA_PWA_DIR: PWA_DIR || '',
				ELEZA_RTPEER_WORK: process.env.ELEZA_RTPEER_WORK || path.resolve(HERE, '../core/rtpeer/.work'),
			},
			stdio: ['pipe', 'pipe', 'inherit'],
		});
		children.push(child);
		user.rtProcess = child;
		let text = '';
		child.stdout.on('data', (chunk) => {
			text += chunk.toString();
			const match = text.match(/READY (\d+)/);
			if (match) {
				user.rtPort = Number(match[1]);
				resolve();
			}
		});
		child.on('exit', (code) => {
			if (!user.rtPort) reject(new Error('the realtime gateway did not start (exit code ' + code + ')'));
			user.rtPort = 0;
		});
	});
}

function stopGateway(user) {
	if (user.rtProcess) {
		user.rtProcess.kill();
		user.rtProcess = null;
		user.rtPort = 0;
	}
}

// ---- helpers

const b64 = (data) => Buffer.from(data).toString('base64url');

function livekitToken(grant, identity, ttl = TOKEN_TTL_S) {
	const now = Math.floor(Date.now() / 1000);
	const signed = b64(JSON.stringify({ alg: 'HS256', typ: 'JWT' })) + '.' + b64(JSON.stringify({
		iss: LIVEKIT_KEY, sub: identity, nbf: now - 10, exp: now + ttl, video: grant,
	}));
	return signed + '.' + crypto.createHmac('sha256', LIVEKIT_SECRET).update(signed).digest('base64url');
}

function livekitRoomService(method, room, body) {
	return new Promise((resolve) => {
		const target = new URL(LIVEKIT_HTTP + '/twirp/livekit.RoomService/' + method);
		const text = JSON.stringify(body);
		const request = http.request({
			host: target.hostname,
			port: target.port,
			path: target.pathname,
			method: 'POST',
			headers: {
				'Content-Type': 'application/json',
				'Content-Length': Buffer.byteLength(text),
				Authorization: 'Bearer ' + livekitToken({ room, roomAdmin: true, roomCreate: true }, 'voice', 60),
			},
		}, (answer) => {
			answer.resume();
			answer.on('end', () => resolve(answer.statusCode));
		});
		request.on('error', () => resolve(0));
		request.end(text);
	});
}

function controlPost(user, route, body) {
	if (!user.rtPort) return;
	const text = JSON.stringify(body);
	const request = http.request({
		host: '127.0.0.1',
		port: user.rtPort,
		path: route,
		method: 'POST',
		headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(text) },
	}, (answer) => answer.resume());
	request.on('error', () => undefined);
	request.end(text);
}

// The voice service's frame: { t, call, state, reason, from } as JSON bytes.
function signal(user, type, call, extra = {}) {
	const frame = { t: type, call: call.id, state: call.state, reason: call.endReason, from: PEER, ...extra };
	controlPost(user, '/control/signal', {
		t: type,
		message: 'CallSignal',
		body: { json: Buffer.from(JSON.stringify(frame)).toString('base64') },
	});
}

const json = (res, status, body) => {
	const text = (status === 204) ? '' : JSON.stringify(body ?? {});
	res.writeHead(status, { 'Content-Type': status >= 400 ? 'application/problem+json' : 'application/json' });
	res.end(text);
};
const problem = (res, status, code, title) => json(res, status, { code, title, status });
const errors = {
	notAllowed: [403, 'call.not_allowed', 'امکان تماس با این کاربر وجود ندارد'],
	busy: [409, 'call.busy', 'کاربر در حال مکالمه است'],
	state: [409, 'call.state', 'این تماس در وضعیت مناسب نیست'],
	notFound: [404, 'call.not_found', 'تماس پیدا نشد'],
};
const fail = (res, error) => problem(res, ...error);

function readBody(req) {
	return new Promise((resolve) => {
		const chunks = [];
		req.on('data', (chunk) => chunks.push(chunk));
		req.on('end', () => {
			const raw = Buffer.concat(chunks);
			try {
				resolve({ raw, value: raw.length ? JSON.parse(raw.toString('utf8')) : {} });
			} catch {
				resolve({ raw, value: {} });
			}
		});
	});
}

// ---- calls (internal/voice/service.go of the backend, in small)

const calls = new Map();
const qosReports = [];
let nextCall = 1;

function joinFor(call, user) {
	const expires = Math.floor(Date.now() / 1000) + TOKEN_TTL_S;
	return {
		call: call.id,
		state: call.state,
		sfu_url: LIVEKIT_URL,
		token: livekitToken(
			{ room: call.room, roomJoin: true, canPublish: true, canSubscribe: true },
			call.id + '-' + user.name),
		token_expires_at: expires,
		// No TURN server in the test bed: the SFU is reached directly.
		turn: { username: expires + ':' + call.id + '-' + user.name, credential: 'unused', urls: [], expires_at: expires },
		peer: PEER,
		relay_only: true,
	};
}

function callOut(call, user) {
	const out = {
		call: call.id,
		state: call.state,
		peer: PEER,
		outgoing: call.caller === user,
		created_at: Math.floor(call.createdAt / 1000),
	};
	if (call.endReason) out.end_reason = call.endReason;
	if (call.connectedAt && call.endedAt) out.duration_s = Math.floor((call.endedAt - call.connectedAt) / 1000);
	if (out.outgoing && call.state === 'ringing' && call.delivered) out.delivered = true;
	return out;
}

function endCall(call, reason) {
	if (call.state === 'ended') return;
	call.state = 'ended';
	call.endReason = reason;
	call.endedAt = Date.now();
	clearTimeout(call.timer);
	for (const user of [call.caller, call.callee]) {
		if (user.call === call) user.call = null;
	}
	// The room is closed with the call, as the backend does.
	livekitRoomService('DeleteRoom', call.room, { room: call.room });
	signal(call.caller, 'call.ended', call);
	signal(call.callee, 'call.ended', call);
	// The record in the chat: a system message sent by the caller.
	const duration = call.connectedAt ? Math.floor((call.endedAt - call.connectedAt) / 1000) : 0;
	const outcome = call.connectedAt ? 'completed' : (reason === 'rejected') ? 'declined' : 'missed';
	for (const user of [call.caller, call.callee]) {
		const seq = user.nextSeq++;
		controlPost(user, '/control/push', {
			kind: 'message',
			data: {
				message: {
					chat: 'c1',
					seq,
					id: 'm_call_' + call.id + '_' + user.name,
					sender: (call.caller === user) ? 'u_me' : PEER,
					kind: 'system',
					system: 'call:' + outcome + ':' + duration,
					call: { outcome, duration_s: duration },
					created_at: call.endedAt,
				},
			},
		});
	}
	log(call, 'ended (' + reason + ')');
}

function log(call, what) {
	if (process.env.QUIET !== '1') {
		console.log(new Date().toISOString().slice(11, 23) + ' ' + call.id + ' ' + what);
	}
}

async function handleCalls(user, req, res, route) {
	const { value: body } = await readBody(req);
	const method = req.method;
	if (route === '/v1/calls' && method === 'POST') {
		const callee = other(user);
		if (body.user_id !== PEER || callee.privacy === 'nobody') return fail(res, errors.notAllowed);
		if (user.call || callee.call || callee.busyElsewhere) return fail(res, errors.busy);
		const id = 'ca' + String(nextCall++).padStart(3, '0');
		const call = {
			id,
			room: 'c' + crypto.randomBytes(8).toString('hex'),
			caller: user,
			callee,
			state: 'ringing',
			endReason: '',
			createdAt: Date.now(),
			acceptedAt: 0,
			connectedAt: 0,
			endedAt: 0,
			delivered: false,
			timer: null,
		};
		calls.set(id, call);
		user.call = call;
		callee.call = call;
		call.timer = setTimeout(() => endCall(call, 'timeout'), RING_TIMEOUT_MS);
		signal(callee, 'call.incoming', call);
		log(call, user.name + ' calls ' + callee.name);
		return json(res, 201, joinFor(call, user));
	}
	if (route === '/v1/calls/incoming' && method === 'GET') {
		const call = user.call;
		return json(res, 200, (call && call.state === 'ringing' && call.callee === user) ? callOut(call, user) : {});
	}
	if (route === '/v1/calls/active' && method === 'GET') {
		const call = user.call;
		if (!call || call.state === 'ended') return fail(res, errors.notFound);
		return json(res, 200, {
			call: call.id,
			state: call.state,
			peer: PEER,
			role: (call.caller === user) ? 'caller' : 'callee',
			created_at: Math.floor(call.createdAt / 1000),
		});
	}
	const match = route.match(/^\/v1\/calls\/([^/]+)(?:\/([a-z]+))?$/);
	const call = match && calls.get(decodeURIComponent(match[1]));
	if (!call || (call.caller !== user && call.callee !== user)) return fail(res, errors.notFound);
	const verb = match[2] || '';
	if (!verb && method === 'GET') return json(res, 200, callOut(call, user));
	if (method !== 'POST') return fail(res, errors.notFound);
	switch (verb) {
	case 'ringing':
		if (call.callee !== user) return fail(res, errors.notFound);
		if (call.state === 'ringing' && !call.delivered) {
			call.delivered = true;
			signal(call.caller, 'call.ringing', call);
			log(call, 'rings at ' + user.name);
		}
		return json(res, 204);
	case 'accept':
		if (call.callee !== user) return fail(res, errors.state);
		if (call.state === 'ringing') {
			call.state = 'accepted';
			call.acceptedAt = Date.now();
			clearTimeout(call.timer);
			call.timer = setTimeout(() => endCall(call, 'network'), CONNECT_GRACE_MS);
			signal(call.caller, 'call.accepted', call);
			signal(call.callee, 'call.answered_elsewhere', call, { device: 'desktop-' + user.name });
			log(call, 'accepted');
		} else if (call.state !== 'accepted' && call.state !== 'connected') {
			return fail(res, errors.state);
		}
		return json(res, 200, joinFor(call, user));
	case 'reject':
		if (call.state === 'ended' && call.endReason === 'rejected') return json(res, 204);
		if (call.callee !== user || call.state !== 'ringing') return fail(res, errors.state);
		endCall(call, 'rejected');
		return json(res, 204);
	case 'cancel':
		if (call.state === 'ended' && call.endReason === 'cancelled') return json(res, 204);
		if (call.caller !== user || call.state !== 'ringing') return fail(res, errors.state);
		endCall(call, 'cancelled');
		return json(res, 204);
	case 'hangup':
		if (call.state === 'ended' && call.endReason === 'hangup') return json(res, 204);
		if (call.state === 'ended') return fail(res, errors.state);
		endCall(call, 'hangup');
		return json(res, 204);
	case 'connected':
		if (call.state === 'accepted') {
			call.state = 'connected';
			call.connectedAt = Date.now();
			clearTimeout(call.timer);
			log(call, 'connected');
		}
		return json(res, 204);
	case 'rejoin':
		if (call.state !== 'accepted' && call.state !== 'connected' && !(call.state === 'ringing' && call.caller === user)) {
			return fail(res, errors.state);
		}
		call.rejoins = (call.rejoins || 0) + 1;
		log(call, user.name + ' rejoins');
		return json(res, 200, joinFor(call, user));
	case 'qos':
		qosReports.push({ call: call.id, user: user.name, ...body });
		return json(res, 204);
	default:
		return fail(res, errors.notFound);
	}
}

async function handleControl(req, res, route) {
	const { value: body } = await readBody(req);
	if (route === '/control/calls') {
		return json(res, 200, {
			calls: [...calls.values()].map((call) => ({
				id: call.id,
				caller: call.caller.name,
				callee: call.callee.name,
				state: call.state,
				end_reason: call.endReason,
				delivered: call.delivered,
				room: call.room,
				rejoins: call.rejoins || 0,
				duration_s: (call.connectedAt && call.endedAt) ? Math.floor((call.endedAt - call.connectedAt) / 1000) : 0,
			})),
			qos: qosReports,
			busy: { a: users.a.call ? users.a.call.id : null, b: users.b.call ? users.b.call.id : null },
		});
	}
	const user = users[String(body.user || '').toLowerCase()];
	if (!user) return problem(res, 400, 'control.user', 'user must be "a" or "b"');
	if (route === '/control/kick') {
		const call = user.call;
		if (!call) return problem(res, 409, 'control.no_call', 'that user is in no call');
		const status = await livekitRoomService('RemoveParticipant', call.room, { room: call.room, identity: call.id + '-' + user.name });
		return json(res, 200, { status });
	}
	if (route === '/control/privacy') {
		user.privacy = (body.calls === 'nobody') ? 'nobody' : 'thread_contacts';
		return json(res, 200, { calls: user.privacy });
	}
	if (route === '/control/busy') {
		user.busyElsewhere = !!body.busy;
		return json(res, 200, { busy: user.busyElsewhere });
	}
	if (route === '/control/signals') {
		if (body.down) {
			stopGateway(user);
		} else if (!user.rtPort) {
			await startGateway(user);
		}
		return json(res, 200, { down: !user.rtPort });
	}
	return problem(res, 404, 'not_found', 'not found');
}

// ---- the two front doors

function forward(user, req, res, port) {
	const upstream = http.request({
		host: '127.0.0.1',
		port,
		method: req.method,
		path: req.url,
		headers: { ...req.headers, host: '127.0.0.1:' + port },
	}, (answer) => {
		res.writeHead(answer.statusCode, answer.headers);
		answer.pipe(res);
	});
	upstream.on('error', () => problem(res, 502, 'proxy.unreachable', 'the mock behind this address is not running'));
	req.pipe(upstream);
}

function serve(user) {
	const server = http.createServer((req, res) => {
		const route = (req.url || '').split('?')[0];
		if (route === '/v1/calls' || route.startsWith('/v1/calls/')) {
			handleCalls(user, req, res, route).catch((error) => {
				console.error(error);
				problem(res, 500, 'mock.failed', 'the mock failed');
			});
		} else if (route.startsWith('/control/')) {
			handleControl(req, res, route).catch((error) => {
				console.error(error);
				problem(res, 500, 'mock.failed', 'the mock failed');
			});
		} else if (route.startsWith('/v2/')) {
			if (!user.rtPort) return problem(res, 503, 'server.unavailable', 'no realtime gateway');
			forward(user, req, res, user.rtPort);
		} else {
			forward(user, req, res, user.apiPort);
		}
	});
	// The realtime connection: the upgrade request goes to the gateway as it came.
	server.on('upgrade', (req, socket, head) => {
		if (!user.rtPort) {
			socket.destroy();
			return;
		}
		const upstream = net.connect(user.rtPort, '127.0.0.1', () => {
			let text = req.method + ' ' + req.url + ' HTTP/1.1\r\n';
			for (let i = 0; i + 1 < req.rawHeaders.length; i += 2) {
				text += req.rawHeaders[i] + ': ' + req.rawHeaders[i + 1] + '\r\n';
			}
			upstream.write(text + '\r\n');
			if (head && head.length) upstream.write(head);
			socket.pipe(upstream);
			upstream.pipe(socket);
		});
		upstream.on('error', () => socket.destroy());
		socket.on('error', () => upstream.destroy());
		// The gateway went away (POST /control/signals): the app sees a dead socket.
		const dropped = setInterval(() => {
			if (!user.rtPort) {
				socket.destroy();
				upstream.destroy();
			}
		}, 200);
		socket.on('close', () => clearInterval(dropped));
	});
	return new Promise((resolve) => server.listen(user.port, '127.0.0.1', resolve));
}

function shutdown(code = 0) {
	for (const child of children) {
		try { child.kill(); } catch { /* gone already */ }
	}
	process.exit(code);
}
process.on('SIGINT', () => shutdown(0));
process.on('SIGTERM', () => shutdown(0));
process.on('exit', () => {
	for (const child of children) {
		try { child.kill(); } catch { /* gone already */ }
	}
});

for (const user of Object.values(users)) startWebMock(user);
try {
	if (!NO_REALTIME) {
		// One after the other: the first one builds what both run.
		for (const user of Object.values(users)) await startGateway(user);
	}
	await Promise.all(Object.values(users).map(serve));
} catch (error) {
	// 77: a prerequisite is missing (the gateway needs node modules and the
	// web client's sources); NO_REALTIME=1 runs without it.
	console.error('mock-calls: ' + (error && error.message ? error.message : error));
	shutdown(77);
}
console.log('READY a=http://127.0.0.1:' + users.a.port + ' b=http://127.0.0.1:' + users.b.port
	+ ' livekit=' + LIVEKIT_URL + (NO_REALTIME ? ' (no realtime)' : ''));
