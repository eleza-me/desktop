#!/usr/bin/env node
// A hostile backend: everything a malicious peer, a broken server or somebody in
// the middle could put into the answers, in front of the web client's mock API
// (pwa/scripts/mock-api.mjs, started on PORT + 10000). The app must neither
// crash nor hang nor allocate absurdly, and must still show something sane.
//
//   node tests/tools/mock-hostile.mjs                 # :8166
//   Eleza --api=http://127.0.0.1:8166 --debug-login=+989121234567:123456 --debug-route=chat/hx
//
// What it serves:
//   /v1/chats            the mock's inbox plus `hx` (hostile messages), `hx2` (wrong-typed
//                        chat document) and entries that are not objects at all
//   /v1/chats/hx/messages
//                        entities out of range / negative / overlapping / inside a
//                        surrogate pair, a 100 000 character text, link-heavy and
//                        "-a-a-a" texts (regex stress), media with zero / negative /
//                        huge dimensions, a broken blurhash, file names with path
//                        traversal, direction overrides, control characters and
//                        device names, link previews and buttons with javascript: and
//                        file: addresses, deeply nested quotes, unknown message kinds,
//                        null and wrong-typed fields, enormous numbers
//   /v1/chats/hx/messages/<seq>/attachment
//                        by seq: a 20000 x 20000 PNG header, an SVG that names a local
//                        file, a redirect loop, a redirect to file:// and to http://
//                        another host, 503 for ever, a truncated JPEG, a body that
//                        never ends, random bytes, an HTML page
//   /v1/users/*/avatar   the same rotation (by a hash of the user id)
//   /v1/sync/difference  unknown update kinds, wrong-typed frames, a huge pts
//   /v1/users, /v1/users/by-handle/evil, /v1/me
//                        handles with spaces and direction overrides, names of 20 000
//                        characters, wrong types
//   /v1/feed/for-you     posts with hostile media, polls with negative and huge counts
//   /v1/notifications    503 with Retry-After: -1 / 1e99 / text (a 5xx storm)
//   /v1/presence         not JSON at all
//   /v2/keys             a listing with junk next to one valid key
//
// HOSTILE_ONLY=media|inbox|... is not needed: every journey works at once.
import http from 'node:http';
import zlib from 'node:zlib';
import fs from 'node:fs';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const PORT = Number(process.env.PORT || 8166);
const UPSTREAM_PORT = Number(process.env.UPSTREAM_PORT || PORT + 10000);
const HERE = path.dirname(fileURLToPath(import.meta.url));
const PWA_MOCK = process.env.PWA_MOCK
	|| ['../../../pwa', '../../../../pwa', '../../../../../pwa']
		.map((dir) => path.resolve(HERE, dir, 'scripts/mock-api.mjs'))
		.find((p) => fs.existsSync(p));

if (PWA_MOCK && !process.env.NO_UPSTREAM) {
	const child = spawn(process.execPath, [PWA_MOCK], { env: { ...process.env, PORT: String(UPSTREAM_PORT) }, stdio: 'ignore' });
	process.on('exit', () => child.kill());
	process.on('SIGINT', () => process.exit(0));
	process.on('SIGTERM', () => process.exit(0));
}

const now = Date.now();
const RLO = '\u202E';
const raw = (res, status, text, headers = {}) => {
	res.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8', ...headers });
	res.end(text);
};
const json = (res, status, body, headers) => raw(res, status, JSON.stringify(body), headers);

function forward(req, res, body, rewrite) {
	const upstream = http.request({
		host: '127.0.0.1',
		port: UPSTREAM_PORT,
		method: req.method,
		path: req.url,
		headers: { ...req.headers, host: `127.0.0.1:${UPSTREAM_PORT}` },
	}, (answer) => {
		const chunks = [];
		answer.on('data', (chunk) => chunks.push(chunk));
		answer.on('end', () => {
			let data = Buffer.concat(chunks);
			const headers = { ...answer.headers };
			if (rewrite && (headers['content-type'] || '').includes('json')) {
				try {
					data = Buffer.from(rewrite(JSON.parse(data.toString('utf8'))));
				} catch (error) { if (process.env.DEBUG) console.error(error); }
			}
			delete headers['content-length'];
			res.writeHead(answer.statusCode, headers);
			res.end(data);
		});
	});
	upstream.on('error', () => json(res, 502, { code: 'proxy.unreachable', title: 'mock api is not running' }));
	upstream.end(body);
}

// ---- hostile messages --------------------------------------------------------------------------

const hx = {
	chat: 'hx', kind: 'direct', state: 'active',
	participants: [{ ref: 'u_me', user_id: 'u_me', me: true }, { ref: 'u_evil', user_id: 'u_evil' }],
	last_seq: 0, peer_read_seq: 0, last_read_seq: 0, can_send: true,
};
const messages = [];
const add = (kind, extra = {}) => {
	const seq = ++hx.last_seq;
	const message = { chat: 'hx', seq, id: `m_hx_${seq}`, sender: (seq % 5) ? 'u_evil' : 'u_me', kind, created_at: now - (400 - seq) * 60_000, ...extra };
	messages.push(message);
	return message;
};
const entity = (type, offset, length, extra = {}) => ({ type, offset, length, ...extra });

// Entities.
add('text', { text: 'out of range entities', entities: [entity('bold', 5, 100000), entity('italic', -10, 5), entity('code', 2147483647, 2147483647), entity('spoiler', 3, -3), entity('text_link', 0, 3, { url: 'javascript:alert(document.cookie)' }), entity('text_link', 4, 2, { url: 'file:///etc/passwd' }), entity('nonsense', 0, 5), entity('bold', 1e30, 1e30)] });
add('text', { text: 'overlap overlap overlap overlap', entities: Array.from({ length: 400 }, (_, i) => entity(['bold', 'italic', 'underline', 'strikethrough', 'code', 'pre', 'spoiler', 'text_link', 'mention', 'hashtag'][i % 10], i % 29, 31 - (i % 31), { url: 'https://eleza.me@evil.example/' + i })) });
add('text', { text: 'A😀B😀C😀D', entities: [entity('bold', 2, 1), entity('italic', 1, 2), entity('text_link', 2, 3, { url: 'https://evil.example' }), entity('spoiler', 5, 1), entity('code', 9, 1)] });
add('text', { text: '\uD83D lone halves \uDE00 \uD83D\uD83D\uDE00\uDE00', entities: [entity('bold', 0, 1), entity('italic', 14, 4)] });
add('text', { text: 'entities of the wrong type', entities: 'bold' });
add('text', { text: 'entities with junk inside', entities: [null, 7, 'x', [], { type: 5, offset: '1', length: '3' }, { type: 'bold' }] });
// Texts.
add('text', { text: 'x'.repeat(100000) });
add('text', { text: 'سلام '.repeat(20000) });
add('text', { text: '-a'.repeat(50000) });
add('text', { text: 'a.io '.repeat(20000) });
add('text', { text: '@sara #tag x@y.co https://a.b/c '.repeat(3000) });
add('text', { text: `direction ${RLO}edirrevo\u202C and \u2066isolates\u2069 \u200F\u200E\u061C zero\u200Bwidth\u2060 \uFEFF\uFFF9\uFFFA\uFFFB` });
add('text', { text: 'control \u0000\u0001\u0007\u0008\u001B[31m red? \u007F\u0085\u2028\u2029 end' });
add('text', { text: '\n'.repeat(5000) + 'after five thousand lines' });
add('text', { text: 'Z̷̧̨̛̛͚̲̦͔̣̠̻̮̝͖̺̬̖̫̼̣̗̝̯̜͖̯͓̰̲͉̼̙̹̱̠̃̈́̂̋̇͗̓͊̈̈̆̃͐̇̓̓͛́̓̎̄̑̒̋̍͌͆̍̾̐̕͘͜͝ͅ'.repeat(200) });
add('text', { text: 12345 });
add('text', { text: null });
add('text', { text: ['array', 'instead'] });
add('text', {});
// Media.
add('photo', { attachment: 'a', media: { w: 0, h: 0, blurhash: 'not a blurhash' } });
add('photo', { attachment: 'a', media: { w: -640, h: -480, blurhash: 'LKO2?U%2Tw=w]~RBVZRi};RPxuwH' } });
add('photo', { attachment: 'a', media: { w: 2147483647, h: 1, blurhash: '~'.repeat(166) } });
add('photo', { attachment: 'a', media: { w: 1, h: 2147483647, blurhash: 'LKO2?U%2Tw=w]~RBVZRi};RPxuw' } });
add('photo', { attachment: 'a', media: { w: 20000, h: 20000, size: 9e18, blurhash: '\u0000\u0001' } });
add('photo', { attachment: 'a', media: { w: '640', h: [480], blurhash: 7 } });
add('photo', { attachment: 'a', media: 'none' });
add('photo', { attachment: 'a' });
add('video', { attachment: 'a', media: { w: 99999, h: 3, duration_ms: -5, blurhash: 'L' } });
add('video', { attachment: 'a', media: { w: 3, h: 99999, duration_ms: 9e18 } });
add('gif', { attachment: 'a', media: { w: 1, h: 1, duration_ms: 1e300 } });
add('video_note', { attachment: 'a', media: { w: -1, h: -1, duration_ms: 0 } });
add('voice', { attachment: 'a', media: { duration_ms: -1, waveform: '!!!not base64!!!' } });
add('voice', { attachment: 'a', media: { duration_ms: 9e15, waveform: Buffer.alloc(100000, 0xff).toString('base64') } });
add('audio', { attachment: 'a', text: `song${RLO}3pm.exe`, media: { duration_ms: 1, mime: 'audio/mpeg' }, audio: { title: 'T'.repeat(20000), performer: 5 } });
add('sticker', { sticker: { id: 's', url: 'file:///etc/passwd', width: 0, height: 0, emoji: '😀' } });
add('sticker', { sticker: { id: 's', url: 'javascript:alert(1)', width: -512, height: 99999999 } });
add('sticker', { sticker: { id: 's', url: '/v1/chats/hx/messages/1/attachment', width: 1e9, height: 1e9 } });
add('sticker', { sticker: 'x' });
// File names.
for (const name of [
	'..\\..\\..\\Windows\\System32\\evil.dll', '../../../../etc/cron.d/evil', '/etc/passwd', 'C:\\Users\\Public\\x.bat',
	`invoice${RLO}fdp.exe`, `photo${RLO}gnp.scr`, 'report.pdf.exe', 'evil.exe.', 'evil.exe ', 'evil.exe::$DATA', 'CON', 'nul.txt', 'COM1.pdf',
	'evil.desktop', 'evil.lnk', 'evil.bat', 'evil.sh', 'evil.html', 'evil.svg', '.bashrc', '.ssh/authorized_keys', '....', ' ', '',
	'new\nline\r\ntab\tnull\u0000bell\u0007.txt', 'n'.repeat(5000) + '.txt', 'emoji😀\uD83D.txt', '~/.config/autostart/x.desktop', '%APPDATA%\\x.exe', '$(reboot).sh', '`id`;rm -rf ~.txt',
]) {
	add('file', { attachment: 'a', text: name, media: { size: 1234, mime: 'application/octet-stream' } });
}
add('file', { attachment: 'a', text: 'size.bin', media: { size: -1, mime: 7 } });
add('file', { attachment: 'a', text: 'size.bin', media: { size: 1e30, mime: 'x'.repeat(10000) } });
// Link previews, buttons, quotes.
add('text', { text: 'preview https://evil.example', link_preview: { url: 'javascript:alert(1)', site_name: 'S'.repeat(5000), title: `${RLO}title`, description: 'd'.repeat(50000), image_url: 'file:///etc/passwd', image_width: -1, image_height: 1e12 } });
add('text', { text: 'preview 2', link_preview: { url: 'file:///C:/Windows/win.ini', image_url: 'javascript:alert(2)', image_width: 2147483647, image_height: 2147483647 } });
add('text', { text: 'preview 3', link_preview: { url: 'https://eleza.me@evil.example/login', title: 'Eleza', image_url: '/v1/chats/hx/messages/3/attachment', image_width: 0, image_height: 0 } });
add('text', { text: 'preview 4', link_preview: 'https://evil.example' });
add('text', { text: 'preview 5', link_preview: { url: 'http://127.0.0.1:1/', image_url: 'http://127.0.0.1:1/x.png', image_width: 100, image_height: 100 } });
add('text', { text: 'buttons', reply_markup: { inline_keyboard: [[{ text: 'js', kind: 'url', url: 'javascript:alert(1)' }, { text: 'file', kind: 'url', url: 'file:///etc/passwd' }, { text: 'logout', kind: 'url', url: 'eleza://debug/logout' }, { text: 'expire', kind: 'url', url: 'eleza://debug/session-expired' }], [], 'x', [null, 5, { text: 9 }], Array.from({ length: 500 }, (_, i) => ({ text: 'b' + i, kind: 'callback' }))] } });
add('text', { text: 'markup of the wrong type', reply_markup: [1, 2, 3] });
let nested = { seq: 1, text: 'innermost', sender: 'u_evil' };
for (let i = 0; i < 300; i++) nested = { seq: 1, text: 'q' + i, sender: 'u_evil', reply_to: nested, quote: { text: 'x', offset: i } };
add('text', { text: 'deeply nested quote', reply_to_seq: 1, reply_to: nested });
add('text', { text: 'quote out of range', reply_to_seq: 3, quote: { text: 'q'.repeat(10000), offset: 2147483647 } });
add('text', { text: 'reply to nowhere', reply_to_seq: -5 });
add('text', { text: 'reply to the future', reply_to_seq: 9e18 });
add('text', { text: 'reactions', reactions: Object.fromEntries(Array.from({ length: 3000 }, (_, i) => ['r' + i, (i % 2) ? -1 : 9e18])) });
add('text', { text: 'reactions of the wrong type', reactions: [1, 2], my_reaction: { a: 1 } });
add('location', { location: { lat: 1e308, lng: -1e308, accuracy_m: -1, label: 'L'.repeat(9000) } });
add('location', { location: { lat: 'north', lng: null } });
add('live_location', { location: { lat: 35.7, lng: 51.4 }, live: { period: -1, until: 9e18 } });
add('call', { call: { outcome: 5, duration_s: -100 } });
add('post', { post: { id: 'nope', author: 'nobody', text: 't'.repeat(40000), media: [{ id: 'm', kind: 'photo', w: -1, h: 0 }] } });
add('contact', { contact: { phone: '+98', name: RLO + 'eman' } });
add('poll', { poll: { question: 'q', options: null } });
add('unknown_kind_from_the_future', { text: 'an unknown kind', payload: { deep: { deeper: [1, [2, [3, [4]]]] } } });
add('', { text: 'an empty kind' });
add(7, { text: 'a numeric kind' });
add('system', { system: { event: 'x'.repeat(5000), actor: null } });
add('text', { text: 'forward', forwarded_from: { user_id: 5, name: ['a'], post: { id: null } }, via_bot: 'x' });
add('text', { text: 'edited in the future', edited_at: 9e18, created_at: -1 });
add('text', { text: 'time as text', created_at: 'yesterday' });
// The same seq twice, seq of the wrong type, no seq.
messages.push({ chat: 'hx', seq: 5, id: 'dup', sender: 'u_evil', kind: 'text', text: 'duplicate seq', created_at: now });
messages.push({ chat: 'hx', seq: '6', id: 'str', sender: 'u_evil', kind: 'text', text: 'seq as a string', created_at: now });
messages.push({ chat: 'other', seq: 7, id: 'other', sender: 'u_evil', kind: 'text', text: 'belongs to another chat', created_at: now });
messages.push({ chat: 'hx', id: 'noseq', sender: 'u_evil', kind: 'text', text: 'no seq' });
messages.push({ chat: 'hx', seq: -3, id: 'neg', sender: 'u_evil', kind: 'text', text: 'negative seq' });
add('text', { text: 'the last message https://evil.example @sara', sender: { id: 'u_evil' } });

const evil = {
	id: 'u_evil', handle: `evil ${RLO}livE\u0000\n@x`, display_name: RLO + 'ELEZA Support ✔ ' + 'n'.repeat(20000), avatar_id: 'av_evil',
	verified: 'yes', bio: 'b'.repeat(100000), link: 'javascript:alert(1)', followers: -5, following: 9e18, posts: 'many',
	birthday: { calendar: 'jalali', month: 99, day: -1, year: 9e9 }, mutuals: [null, 5, { id: 7 }], mutuals_total: -1, since: 'x',
};

// ---- hostile media -----------------------------------------------------------------------------

function pngHeader(width, height) {
	const chunk = (type, data) => {
		const head = Buffer.alloc(8);
		head.writeUInt32BE(data.length, 0);
		head.write(type, 4, 'ascii');
		const crc = Buffer.alloc(4);
		crc.writeUInt32BE(zlib.crc32(Buffer.concat([head.subarray(4), data])) >>> 0, 0);
		return Buffer.concat([head, data, crc]);
	};
	const ihdr = Buffer.alloc(13);
	ihdr.writeUInt32BE(width, 0);
	ihdr.writeUInt32BE(height, 4);
	ihdr[8] = 8; ihdr[9] = 6;
	// One short IDAT: the decoder is promised 1.6 GB of pixels and gets none.
	return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(Buffer.alloc(64))), chunk('IEND', Buffer.alloc(0))]);
}
const SVG = Buffer.from('<?xml version="1.0"?><!DOCTYPE svg [<!ENTITY x SYSTEM "file:///etc/passwd">]><svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="400" height="300"><rect width="400" height="300" fill="red"/><image width="400" height="300" xlink:href="file:///etc/passwd"/><text y="20">&x;</text></svg>');
let storm = 0;
function media(req, res, pick) {
	switch (pick % 12) {
	case 0: res.writeHead(200, { 'Content-Type': 'image/png' }); return res.end(pngHeader(20000, 20000));
	case 1: res.writeHead(200, { 'Content-Type': 'image/png' }); return res.end(SVG);
	case 2: res.writeHead(302, { Location: req.url }); return res.end();
	case 3: res.writeHead(302, { Location: 'file:///etc/passwd' }); return res.end();
	case 4: res.writeHead(302, { Location: 'http://127.0.0.1:1/unreachable' }); return res.end();
	case 5: res.writeHead(503, { 'Retry-After': ['-1', '1e99', 'soon', '0'][storm++ % 4], 'Content-Type': 'application/json' }); return res.end('{"code":"server.overloaded","title":"overloaded"}');
	case 6: res.writeHead(200, { 'Content-Type': 'image/jpeg' }); return res.end(Buffer.from('ffd8ffe000104a46494600010100000100010000ffdb004300', 'hex'));
	case 7: { // never ends (until the client gives up or 300 MB went out)
		res.writeHead(200, { 'Content-Type': 'image/png' });
		const block = Buffer.alloc(1 << 20, 0x41);
		let sent = 0;
		const pump = () => {
			while (sent < 300 && !res.destroyed) {
				sent++;
				if (!res.write(block)) return res.once('drain', pump);
			}
			res.end();
		};
		return pump();
	}
	case 8: res.writeHead(200, { 'Content-Type': 'image/png' }); return res.end(Buffer.from(Array.from({ length: 4096 }, (_, i) => (i * 131 + 7) & 255)));
	case 9: res.writeHead(200, { 'Content-Type': 'text/html' }); return res.end('<html><script>alert(1)</script></html>');
	case 10: res.writeHead(200, { 'Content-Type': 'image/png' }); return res.end(pngHeader(2147483647, 2147483647));
	default: res.writeHead(200, { 'Content-Type': 'image/gif' }); return res.end(Buffer.from('4749463839610000000000002c00000000ffffffff00', 'hex'));
	}
}
const hashOf = (text) => [...text].reduce((sum, ch) => (sum * 31 + ch.charCodeAt(0)) >>> 0, 7);

// ---- server ------------------------------------------------------------------------------------

const server = http.createServer(async (req, res) => {
	const chunks = [];
	for await (const chunk of req) chunks.push(chunk);
	const body = Buffer.concat(chunks);
	const url = new URL(req.url, `http://127.0.0.1:${PORT}`);
	const p = url.pathname;
	const m = req.method;
	let mm;

	if (p === '/v2/keys') {
		return raw(res, 200, '{"keys":[null,7,{"id":0,"pub":"00"},{"id":999,"pub":"' + 'ab'.repeat(32) + '"},{"id":1e99,"pub":5},{"id":2,"pub":"' + 'zz'.repeat(32) + '"},{"id":3,"pub":"' + 'cd'.repeat(32) + '"}],"preferred":1e300,"protocol":7}');
	}
	if (p === '/v1/chats' && m === 'GET') {
		return forward(req, res, body, (answer) => {
			const section = url.searchParams.get('section') || 'message';
			if (section !== 'message') return JSON.stringify(answer);
			const last = messages[messages.length - 1];
			const list = [
				{ chat: 'hx', section, state: 'active', last_seq: hx.last_seq, unread: 2147483647, pinned: 'yes', last_activity_at: now + 60_000, last_message: last, chat_info: hx },
				{ chat: 'hx2', section, state: 7, last_seq: 'many', unread: -5, last_activity_at: 'now', last_message: 'hello', chat_info: { chat: 'hx2', kind: 5, participants: 'everyone', pins: 'x', draft: 7, conversation: [] } },
				{ chat: 'hx3', section, state: 'active', last_seq: 1, unread: 1, last_activity_at: 9e18, last_message: { chat: 'hx3', seq: 1, sender: 'u_evil', kind: 'file', text: `x${RLO}fdp.exe`, created_at: 9e18 }, chat_info: { chat: 'hx3', kind: 'conversation', participants: [], conversation: { id: 'k', topic: 't'.repeat(30000), speaker: null, listener: 'x', expires_at: -1 } } },
				...(answer.chats || []),
			];
			// Entries that are not chats at all, and numbers JSON.stringify would not write.
			return '{"chats":[null,5,"x",[],{},{"chat":null},{"chat":""},{"chat":"dup"},{"chat":"dup"},' + list.map((x) => JSON.stringify(x)).join(',') + '],"next_cursor":12345,"total":1e300,"unread":-1e300}';
		});
	}
	if (p === '/v1/chats/hx' && m === 'GET') return json(res, 200, hx);
	if (p === '/v1/chats/hx2' && m === 'GET') return raw(res, 200, '{"chat":"hx2","kind":5,"participants":{"a":1},"last_seq":1e400}');
	if (p === '/v1/chats/hx3' && m === 'GET') return raw(res, 200, '[]');
	if ((mm = p.match(/^\/v1\/chats\/(hx|hx2|hx3)\/messages$/)) && m === 'GET') {
		if (mm[1] !== 'hx') return raw(res, 200, '{"chat":null,"messages":{"0":"x"},"has_more_before":"yes"}');
		const before = Number(url.searchParams.get('before') || 0);
		const after = Number(url.searchParams.get('after') || 0);
		const limit = Number(url.searchParams.get('limit') || 50);
		let list = messages;
		if (before) list = list.filter((x) => x.seq < before);
		if (after) list = list.filter((x) => x.seq > after);
		const page = (after ? list.slice(0, limit) : list.slice(-limit)).reverse();
		return json(res, 200, { chat: hx, messages: page, has_more_before: page.length >= limit, has_more_after: 'maybe' });
	}
	if ((mm = p.match(/^\/v1\/chats\/hx[23]?\/messages\/(-?\d+)\/attachment$/))) return media(req, res, Number(mm[1]));
	if ((mm = p.match(/^\/v1\/chats\/hx[23]?\/(read|draft|capability|pin|member)$/))) return raw(res, 200, '{"capability":5,"expires_at":"never"}');
	if ((mm = p.match(/^\/v1\/users\/([^/]+)\/avatar$/))) return media(req, res, hashOf(mm[1]));
	if (p === '/v1/users/by-handle/evil' || p === '/v1/users/u_evil') return json(res, 200, evil);
	if (p === '/v1/users' || p === '/v1/users/cards') {
		return forward(req, res, body, (answer) => {
			const users = answer.users || [];
			if (Array.isArray(users)) users.push(evil, null, 5, { id: null }, { id: 'u_evil2', handle: 7, display_name: ['x'] });
			return JSON.stringify({ ...answer, users });
		});
	}
	if (p === '/v1/sync/difference') {
		return raw(res, 200, '{"pts":' + (url.searchParams.get('pts') === '0' ? '1e400' : '1e300') + ',"updates":[null,5,"x",{"kind":"from_the_future","pts":3,"data":{"x":[1,2,3]}},'
			+ '{"kind":"message","pts":"4","chat":"hx","message":"not an object"},{"kind":"message","chat":"hx","seq":-1,"message":{"chat":"hx","seq":-1,"kind":"text","text":"negative"}},'
			+ '{"kind":"read","data":{"read":{"chat":"hx","seq":1e30}}},{"kind":"deleted","chat":"hx","seq":[1,2]},{"kind":"reaction","chat":"nowhere","seq":1,"data":null},'
			+ '{"kind":"state","data":{"state":{"chat":"hx","state":5}}},{"kind":"member","chat":"hx","chat_info":"x"},{"removed":true},{"removed":true,"chat":"hx2"}],"has_more":"yes","too_long":7}');
	}
	// Every third answer is not JSON at all (1e400 does not parse), the others are absurd.
	if (p === '/v1/sync/state') return raw(res, 200, (storm++ % 3 === 0) ? '{"pts":-1e400}' : '{"pts":9e18,"extra":[[[[[[[[[[1]]]]]]]]]]}');
	if (p === '/v1/presence') return raw(res, 200, '<html>502 Bad Gateway</html>');
	if (p.startsWith('/v1/notifications')) {
		return raw(res, 503, '{"code":"server.overloaded","title":"overloaded","retryable":"yes"}', { 'Retry-After': ['-1', '1e99', 'soon', '0'][storm++ % 4] });
	}
	if (p === '/v1/feed/for-you' || p === '/v1/feed/following') {
		return forward(req, res, body, (answer) => {
			const posts = answer.posts || answer.items || [];
			const hostile = [
				{ id: 'hp1', author: 'u_evil', text: `${RLO}tsop elitsoh ` + 'x'.repeat(30000), created_at: 9e18, counts: { likes: -1, replies: 9e18, reposts: 'x', views: 1e30 }, my: 'x',
					media: [{ id: 'm1', kind: 'photo', w: 0, h: 0, blurhash: '???' }, { id: 'm2', kind: 'video', w: -1, h: 1e9, duration_ms: -1 }, null, 5, { id: 'm3', kind: 'photo', w: 2147483647, h: 2147483647 }],
					poll: { question: 'q'.repeat(9000), options: [{ text: 'a', votes: -1 }, { text: 'b', votes: 9e18 }, null, 'c'], total: -5, ends_at: 'x' },
					entities: [{ type: 'text_link', offset: -1, length: 1e9, url: 'javascript:alert(1)' }], link_preview: { url: 'file:///etc/passwd', image_url: 'javascript:1' } },
				{ id: 'hp2', author: { id: 'u_evil' }, text: 5, media: 'x', poll: 'x', quoted: { id: 'hp2', quoted: { id: 'hp2' } }, thread_parts: 'x' },
				{ id: 'hp3', author: 'u_evil', text: 'a.io '.repeat(8000), media: Array.from({ length: 300 }, (_, i) => ({ id: 'mm' + i, kind: 'photo', w: 10, h: 10 })) },
			];
			if (Array.isArray(posts)) posts.unshift(...hostile);
			const authors = answer.authors;
			if (Array.isArray(authors)) authors.push(evil); else if (authors && typeof authors === 'object') authors.u_evil = evil;
			return JSON.stringify({ ...answer, posts, items: answer.items ? posts : undefined, authors });
		});
	}
	if ((mm = p.match(/^\/v1\/posts\/(hp\d|[^/]+)\/media\/([^/]+)$/)) && p.includes('/hp')) return media(req, res, hashOf(mm[2]));
	if (p === '/v1/link-preview') return json(res, 200, { url: 'javascript:alert(1)', title: RLO + 'x', image_url: 'file:///etc/passwd', image_width: -1, image_height: -1 });
	if (p === '/v1/client-config') {
		return forward(req, res, body, (answer) => JSON.stringify({ ...answer, 'conversation.topic_max_chars': -1, 'conversation.durations': 'x', 'conversation.categories': [null, 5, { slug: 7 }], 'chat.max_text': 1e30, update_url: 'javascript:alert(1)' }));
	}
	forward(req, res, body);
});
server.listen(PORT, '127.0.0.1', () => console.log(`hostile mock on http://127.0.0.1:${PORT} (upstream :${UPSTREAM_PORT})`));
