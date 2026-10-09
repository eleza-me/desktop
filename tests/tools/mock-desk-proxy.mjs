#!/usr/bin/env node
// A proxy in front of the web client's mock API (pwa/scripts/mock-api.mjs) with
// what the chat list and the settings screens need and the mock does not have:
//
//   chat list   an archived chat (c7), a message request (c8), a draft on c2;
//               PUT /v1/me/pinned-chats; MANY=<n> adds n more chats (scrolling);
//               EMPTY=1 -> an empty inbox; FAIL_INBOX=1 -> 500 on GET /v1/chats;
//               SLOW=<ms> delays the inbox (the skeleton)
//   two-step    GET/POST /v1/account/two-step… (stateful). TWO_STEP=on starts
//               enabled (password "secret12"); confirm code 111111 is wrong,
//               999999 is expired; COOLDOWN=1 -> 403 account.sensitive_cooldown
//   export      POST /v1/account/otp (422 export.email_required without a
//               recovery e-mail), POST /v1/me/exports (code 000000 is wrong),
//               GET /v1/me/exports/latest, GET /v1/me/exports/<id>/download
//   sessions    four devices with city / last_seen_at; DELETE and revoke-others
//               really remove them
//   config      feature.two_step, auth.two_step.min_password_length = 8
//
//   PORT=8101 node <pwa>/scripts/mock-api.mjs &
//   PORT=8111 TARGET=http://127.0.0.1:8101 node tests/tools/mock-desk-proxy.mjs &
import http from 'node:http';

const PORT = Number(process.env.PORT || 8002);
const TARGET = new URL(process.env.TARGET || 'http://127.0.0.1:8000');
const now = Date.now();

const json = (res, status, body) => {
	const text = (status === 204) ? '' : JSON.stringify(body ?? {});
	res.writeHead(status, { 'Content-Type': status >= 400 ? 'application/problem+json' : 'application/json' });
	res.end(text);
};
const problem = (res, status, code, title, extra = {}) => json(res, status, { code, title, status, ...extra });

function forward(req, res, body, rewrite) {
	const upstream = http.request({
		host: TARGET.hostname,
		port: TARGET.port,
		method: req.method,
		path: req.url,
		headers: { ...req.headers, host: TARGET.host },
	}, (answer) => {
		const chunks = [];
		answer.on('data', (chunk) => chunks.push(chunk));
		answer.on('end', () => {
			let data = Buffer.concat(chunks);
			const headers = { ...answer.headers };
			if (rewrite && (headers['content-type'] || '').includes('json')) {
				try {
					data = Buffer.from(JSON.stringify(rewrite(JSON.parse(data.toString('utf8')))));
				} catch (error) { if (process.env.DEBUG) console.error(error); }
			}
			delete headers['content-length'];
			res.writeHead(answer.statusCode, headers);
			res.end(data);
		});
	});
	upstream.on('error', () => problem(res, 502, 'proxy.unreachable', 'mock api is not running'));
	upstream.end(body);
}

// ---- chats
const extraUsers = {
	u_nika: { id: 'u_nika', handle: 'nika', display_name: 'نیکا موسوی' },
	u_reza: { id: 'u_reza', handle: 'reza.k', display_name: 'رضا کریمی', verified: true },
};
const direct = (chat, user) => ({ chat, kind: 'direct', state: 'active', participants: [{ ref: 'u_me', user_id: 'u_me', me: true }, { ref: user, user_id: user }], last_seq: 1, peer_read_seq: 0, last_read_seq: 0, can_send: true });
const extraChats = {
	c7: { ...direct('c7', 'u_nika'), archived: true, last_read_seq: 1 },
	c8: { ...direct('c8', 'u_reza'), request: 'incoming' },
};
const extraMessages = {
	c7: [{ chat: 'c7', seq: 1, id: 'm_c7_1', sender: 'u_nika', kind: 'voice', attachment: 'media7', media: { duration_ms: 12000 }, created_at: now - 86400_000 * 9 }],
	c8: [{ chat: 'c8', seq: 1, id: 'm_c8_1', sender: 'u_reza', kind: 'text', text: 'سلام، می‌تونم یه سؤال بپرسم؟', created_at: now - 3600_000 * 2 }],
};
const entry = (id, section, extra = {}) => {
	const chat = extraChats[id];
	const last = extraMessages[id].at(-1);
	return { chat: id, section, state: 'active', last_seq: chat.last_seq, unread: 0, last_activity_at: last.created_at, last_message: last, chat_info: chat, ...extra };
};
const many = Number(process.env.MANY || 0);
for (let i = 0; i < many; ++i) {
	const id = 'x' + i;
	const user = (i % 2) ? 'u_nika' : 'u_reza';
	extraChats[id] = direct(id, user);
	extraMessages[id] = [{ chat: id, seq: 1, id: 'm_' + id, sender: (i % 3) ? user : 'u_me', kind: 'text', text: 'پیام شمارهٔ ' + (i + 1) + ' برای آزمودن پیمایش فهرست', created_at: now - 86400_000 * 12 - i * 3600_000 }];
}

function rewriteInbox(section, body) {
	if (process.env.EMPTY) return { chats: [], next_cursor: '' };
	const chats = body.chats || [];
	if (section === 'message') {
		for (const item of chats) {
			if (item.chat === 'c2') item.draft = { text: 'فردا ساعت ۵ می‌بینمت، فقط', updated_at: Math.floor(now / 1000) - 1200 };
		}
		chats.push(entry('c7', 'message', { archived: true }));
		for (let i = 0; i < many; ++i) chats.push(entry('x' + i, 'message'));
	} else if (section === 'request') {
		chats.push(entry('c8', 'request', { unread: 1 }));
	}
	return { ...body, chats };
}

// ---- two-step verification
const twoStep = (process.env.TWO_STEP === 'on')
	? { enabled: true, password: 'secret12', hint: 'اسم اولین معلم', email: 'hadi@example.com', pending: null }
	: { enabled: false, password: '', hint: '', email: '', pending: null };
const mask = (email) => email ? (email[0] + '•••@' + email.split('@')[1]) : '';
const validEmail = (email) => /^[\x21-\x7e]+@[a-z0-9.-]+\.[a-z]{2,}$/i.test(email || '') && email.length <= 254;
const challenge = (email) => ({ code_length: 6, expires_in: 600, resend_after: 60, email: mask(email) });

function handleTwoStep(req, res, path, body) {
	if (process.env.TWO_STEP === 'off') return problem(res, 404, 'not_found', 'پیدا نشد');
	if (path === '/v1/account/two-step' && req.method === 'GET') {
		const out = { enabled: twoStep.enabled, pending: !!twoStep.pending };
		if (twoStep.enabled) Object.assign(out, { hint: twoStep.hint || undefined, recovery_email: mask(twoStep.email) });
		if (twoStep.pending) out.pending_email = mask(twoStep.pending.email);
		return json(res, 200, out);
	}
	const cooldown = () => process.env.COOLDOWN ? problem(res, 403, 'account.sensitive_cooldown', 'این کار روی دستگاه جدید کمی بعد ممکن است') : null;
	const weak = (password) => (password || '').length < 8 || (password || '').length > 256;
	const badHint = (hint, password) => hint && (hint.length > 64 || /\n/.test(hint) || hint.toLowerCase().includes((password || '').toLowerCase()));
	if (path === '/v1/account/two-step/setup') {
		if (twoStep.enabled) return problem(res, 409, 'two_step.already_enabled', 'تأیید دومرحله‌ای روشن است');
		if (cooldown()) return;
		if (weak(body.password)) return problem(res, 400, 'two_step.password_weak', 'رمز کوتاه است');
		if (badHint(body.hint, body.password)) return problem(res, 400, 'two_step.hint_invalid', 'راهنما نباید خود رمز باشد');
		if (!validEmail(body.recovery_email)) return problem(res, 400, 'two_step.email_invalid', 'ایمیل درست نیست');
		twoStep.pending = { kind: 'setup', password: body.password, hint: body.hint || '', email: body.recovery_email };
		return json(res, 200, challenge(body.recovery_email));
	}
	if (path === '/v1/account/two-step/confirm') {
		if (!twoStep.pending) return problem(res, 409, 'two_step.no_pending', 'چیزی برای تأیید نیست');
		if (body.code === '111111') return problem(res, 400, 'two_step.code_invalid', 'کد اشتباه است');
		if (body.code === '999999') return problem(res, 400, 'two_step.code_expired', 'کد منقضی شده است');
		if (twoStep.pending.kind === 'setup') Object.assign(twoStep, { enabled: true, password: twoStep.pending.password, hint: twoStep.pending.hint });
		twoStep.email = twoStep.pending.email;
		twoStep.pending = null;
		return json(res, 204);
	}
	if (!twoStep.enabled) return problem(res, 409, 'two_step.not_enabled', 'تأیید دومرحله‌ای خاموش است');
	if (path === '/v1/account/two-step/password') {
		if (body.current_password !== twoStep.password) return problem(res, 403, 'two_step.password_invalid', 'رمز اشتباه است');
		if (weak(body.new_password)) return problem(res, 400, 'two_step.password_weak', 'رمز کوتاه است');
		if (badHint(body.hint, body.new_password)) return problem(res, 400, 'two_step.hint_invalid', 'راهنما نباید خود رمز باشد');
		Object.assign(twoStep, { password: body.new_password, hint: body.hint || '' });
		return json(res, 204);
	}
	if (path === '/v1/account/two-step/email') {
		if (body.password !== twoStep.password) return problem(res, 403, 'two_step.password_invalid', 'رمز اشتباه است');
		if (cooldown()) return;
		if (!validEmail(body.recovery_email)) return problem(res, 400, 'two_step.email_invalid', 'ایمیل درست نیست');
		twoStep.pending = { kind: 'email', email: body.recovery_email };
		return json(res, 200, challenge(body.recovery_email));
	}
	if (path === '/v1/account/two-step/disable') {
		if (body.password !== twoStep.password) return problem(res, 403, 'two_step.password_invalid', 'رمز اشتباه است');
		Object.assign(twoStep, { enabled: false, password: '', hint: '', email: '', pending: null });
		return json(res, 204);
	}
	return problem(res, 404, 'not_found', 'پیدا نشد');
}

// ---- data export
let latestExport = (process.env.EXPORT === 'ready')
	? { id: 'ex0', state: 'ready', requested_at: now - 3 * 3600_000, ready_at: now - 2 * 3600_000, expires_at: now + 70 * 3600_000, downloads_left: 3, size_bytes: 482_113, emailed_at: now - 2 * 3600_000 + 90_000 }
	: null;

// ---- sessions
let sessions = [
	{ id: 's1', device_name: 'Eleza Desktop', platform: 'desktop', app_version: '0.1.0', city: 'Tehran', current: true, last_seen_at: new Date(now).toISOString() },
	{ id: 's2', device_name: 'iPhone 15', platform: 'ios', app_version: '1.4.0', city: 'Tehran', last_seen_at: new Date(now - 40 * 60_000).toISOString() },
	{ id: 's3', device_name: 'Samsung Galaxy S23', platform: 'android', app_version: '1.3.2', city: 'Shiraz', last_seen_at: new Date(now - 26 * 3600_000).toISOString() },
	{ id: 's4', device_name: 'Chrome · Windows', platform: 'web', app_version: '0.9.1', city: '', last_seen_at: new Date(now - 9 * 86400_000).toISOString() },
];

http.createServer((req, res) => {
	const chunks = [];
	req.on('data', (chunk) => chunks.push(chunk));
	req.on('end', () => {
		const raw = Buffer.concat(chunks);
		const url = new URL(req.url, 'http://x');
		const path = url.pathname;
		const method = req.method;
		let body = {};
		try { body = raw.length ? JSON.parse(raw.toString('utf8')) : {}; } catch { /* */ }
		let m;

		if (path === '/v1/client-config') {
			return forward(req, res, raw, (config) => ({
				...config,
				'feature.two_step': process.env.TWO_STEP !== 'off',
				'feature.drafts': true,
				'auth.two_step.min_password_length': 8,
				'chat.inbox.max_pinned': 3,
			}));
		}
		if (path === '/v1/chats' && method === 'GET') {
			if (process.env.FAIL_INBOX) return problem(res, 500, 'internal', 'خطای سرور');
			if (process.env.SLOW) {
				return void setTimeout(() => forward(req, res, raw, (answer) => rewriteInbox(url.searchParams.get('section') || 'message', answer)), Number(process.env.SLOW));
			}
			return forward(req, res, raw, (answer) => rewriteInbox(url.searchParams.get('section') || 'message', answer));
		}
		if ((m = path.match(/^\/v1\/chats\/([^/]+)$/)) && extraChats[m[1]] && method === 'GET') return json(res, 200, extraChats[m[1]]);
		if ((m = path.match(/^\/v1\/chats\/([^/]+)\/messages$/)) && extraChats[m[1]] && method === 'GET') {
			return json(res, 200, { chat: extraChats[m[1]], messages: [...extraMessages[m[1]]].reverse(), has_more_before: false });
		}
		if ((m = path.match(/^\/v1\/chats\/([^/]+)\//)) && extraChats[m[1]]) return json(res, 204);
		if (path === '/v1/me/pinned-chats') return json(res, 204);
		if (path === '/v1/users' && method === 'GET') {
			const ids = (url.searchParams.get('ids') || '').split(',');
			return forward(req, res, raw, (answer) => {
				const users = answer.users || [];
				for (const id of ids) {
					if (extraUsers[id] && !users.some((user) => user.id === id)) users.push(extraUsers[id]);
				}
				return { ...answer, users };
			});
		}

		if (path.startsWith('/v1/account/two-step')) return handleTwoStep(req, res, path, body);

		if (path === '/v1/account/otp') {
			if (body.purpose === 'data_export' && !(twoStep.enabled && twoStep.email)) {
				return problem(res, 422, 'export.email_required', 'اول ایمیل بازیابی را تنظیم کن');
			}
			if (process.env.COOLDOWN) return problem(res, 403, 'account.sensitive_cooldown', 'این کار روی دستگاه جدید کمی بعد ممکن است');
			return json(res, 200, { challenge_id: 'acc1', code_length: 6, resend_after: 60, expires_in: 300 });
		}
		if (path === '/v1/me/exports' && method === 'POST') {
			if (!(twoStep.enabled && twoStep.email)) return problem(res, 422, 'export.email_required', 'اول ایمیل بازیابی را تنظیم کن');
			if (body.challenge_id !== 'acc1' || body.code === '000000' || !/^\d{6}$/.test(body.code || '')) return problem(res, 400, 'otp.invalid', 'کد اشتباه است');
			latestExport = { id: 'ex1', state: 'pending', requested_at: Date.now(), next_allowed_at: Date.now() + 86400_000 };
			return json(res, 200, latestExport);
		}
		if (path === '/v1/me/exports/latest') {
			return latestExport ? json(res, 200, latestExport) : problem(res, 404, 'export.not_found', 'خروجی‌ای نیست');
		}
		if ((m = path.match(/^\/v1\/me\/exports\/([^/]+)\/download$/))) {
			if (latestExport) latestExport.downloads_left = Math.max((latestExport.downloads_left || 1) - 1, 0);
			return json(res, 200, { profile: { handle: 'hadi' }, exported_at: new Date().toISOString(), chats: [], posts: [] });
		}

		if (path === '/v1/sessions' && method === 'GET') return json(res, 200, sessions);
		if (path === '/v1/sessions/revoke-others') {
			sessions = sessions.filter((session) => session.current);
			return json(res, 204);
		}
		if ((m = path.match(/^\/v1\/sessions\/([^/]+)$/)) && method === 'DELETE') {
			sessions = sessions.filter((session) => session.id !== m[1]);
			return json(res, 204);
		}
		return forward(req, res, raw);
	});
}).listen(PORT, '127.0.0.1', () => console.log(`mock desk proxy on http://127.0.0.1:${PORT} -> ${TARGET.origin}`));
