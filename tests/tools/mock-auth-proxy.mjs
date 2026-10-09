#!/usr/bin/env node
// A thin proxy in front of the web client's mock API (pwa/scripts/mock-api.mjs)
// that adds the sign-in cases the mock does not have, for testing the intro:
//
//   code 000000                       -> 400 otp.invalid (the wrong-code state)
//   code 333333                       -> a brand new account (the profile step)
//   POST /v1/auth/two-step/recover    -> a recovery challenge (any 6 digits confirm,
//                                        111111 is wrong)
//   GET  /v1/client-config?upgrade    -> start with UPGRADE=1 to require a newer build
//
//   node <pwa>/scripts/mock-api.mjs &                 # :8000
//   node tests/tools/mock-auth-proxy.mjs &            # :8001 -> :8000
//   Eleza --api=http://127.0.0.1:8001 ...
import http from 'node:http';

const PORT = Number(process.env.PORT || 8001);
const TARGET = new URL(process.env.TARGET || 'http://127.0.0.1:8000');
let newUser = false;

const json = (res, status, body) => {
	const text = JSON.stringify(body);
	res.writeHead(status, { 'Content-Type': status >= 400 ? 'application/problem+json' : 'application/json' });
	res.end(text);
};

function forward(req, res, body, rewrite) {
	const options = {
		host: TARGET.hostname,
		port: TARGET.port,
		method: req.method,
		path: req.url,
		headers: { ...req.headers, host: TARGET.host },
	};
	const upstream = http.request(options, (answer) => {
		const chunks = [];
		answer.on('data', (chunk) => chunks.push(chunk));
		answer.on('end', () => {
			let data = Buffer.concat(chunks);
			const headers = { ...answer.headers };
			if (rewrite && (headers['content-type'] || '').includes('json')) {
				try {
					data = Buffer.from(JSON.stringify(rewrite(JSON.parse(data.toString('utf8')))));
				} catch { /* not json after all */ }
			}
			delete headers['content-length'];
			res.writeHead(answer.statusCode, headers);
			res.end(data);
		});
	});
	upstream.on('error', () => json(res, 502, { code: 'proxy.unreachable', title: 'mock api is not running' }));
	upstream.end(body);
}

http.createServer((req, res) => {
	const chunks = [];
	req.on('data', (chunk) => chunks.push(chunk));
	req.on('end', () => {
		const body = Buffer.concat(chunks);
		const path = new URL(req.url, 'http://x').pathname;
		let parsed = {};
		try { parsed = body.length ? JSON.parse(body.toString('utf8')) : {}; } catch { /* */ }
		if (path === '/v1/auth/otp/verify' && parsed.code === '000000') {
			return json(res, 400, { code: 'otp.invalid', title: 'کد اشتباه است' });
		}
		if (path === '/v1/auth/otp/verify' && parsed.code === '333333') {
			newUser = true;
			return forward(req, res, body, (tokens) => ({ ...tokens, is_new_user: true }));
		}
		if (path === '/v1/auth/two-step/recover') {
			return json(res, 200, { code_length: 6, expires_in: 600, resend_after: 60, email: 'h•••@gmail.com' });
		}
		if (path === '/v1/auth/two-step/recover/confirm') {
			if (parsed.code === '111111') return json(res, 400, { code: 'two_step.code_invalid', title: 'کد اشتباه است' });
			req.url = '/v1/auth/otp/verify';
			return forward(req, res, Buffer.from(JSON.stringify({ code: '123456' })));
		}
		if (path === '/v1/client-config' && process.env.UPGRADE) {
			return forward(req, res, body, (config) => ({
				...config,
				'api.min_client_version.desktop': '99.0.0',
				'api.update_url.desktop': 'https://eleza.me/download',
			}));
		}
		if (path === '/v1/me' && req.method === 'GET' && newUser) {
			return forward(req, res, body, (me) => (me.handle === 'hadi' ? { ...me, handle: '', display_name: '' } : me));
		}
		if (path === '/v1/me/handle') newUser = false;
		forward(req, res, body);
	});
}).listen(PORT, '127.0.0.1', () => console.log(`mock auth proxy on http://127.0.0.1:${PORT} -> ${TARGET.origin}`));
