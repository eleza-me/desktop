#!/usr/bin/env node
// A proxy in front of the web client's mock API (pwa/scripts/mock-api.mjs) that
// makes the social graph stateful and adds the profile cases the mock does not
// have, for testing the profile screens:
//
//   GET /v1/users/<id>                 a profile by id (the mock only has by-handle)
//   follow / unfollow / block / mute   remembered per user; a private account
//                                      answers "requested"
//   u/private.pending                  a private account with my request pending
//   u/blocked.user                     somebody I blocked
//   u/mina                             a mutual follow with an avatar and a long bio
//   u/gone                             a deleted account
//   u/nobody                           404 (unavailable / blocked me)
//   followers / following of u_sara    260 people over three pages (cursor)
//   GET /v1/me/follow-requests         three requests; accept / decline remove them
//   GET /v1/handles/<h>/availability   "taken*" is taken, "slow*" answers after 2 s,
//                                      "fail*" answers 500
//   PUT /v1/me/handle                  "reserved" -> 400 handle.reserved_for_bots
//   PUT /v1/me/birthday                year 1300 -> 409 birthday.change_limit
//   uploads                            the part PUT takes SLOW_UPLOAD ms (default 0)
//   GET /v1/users/<id>/posts?tab=replies   empty (the «no replies» state)
//   GET /v1/users/u_nika/posts         60 posts over three pages (next_before)
//   GET /v1/me/bookmarks               two saved posts
//
//   PORT=8105 node <pwa>/scripts/mock-api.mjs &
//   PORT=8115 TARGET=http://127.0.0.1:8105 node tests/tools/mock-profile-proxy.mjs &
//   Eleza --api=http://127.0.0.1:8115 ...
import http from 'node:http';
import zlib from 'node:zlib';

const PORT = Number(process.env.PORT || 8115);
const TARGET = new URL(process.env.TARGET || 'http://127.0.0.1:8105');
const SLOW_UPLOAD = Number(process.env.SLOW_UPLOAD || 0);

const handles = { u_me: 'hadi', u_sara: 'sara', u_ali: 'ali.r', u_bot: 'stickerbot', u_nika: 'nika', u_reza: 'reza.k' };
const extra = {
	u_priv: { id: 'u_priv', handle: 'private.pending', display_name: 'پرنیان صادقی', bio: 'فقط دوستان نزدیک', followers: 48, following: 51, posts: 9, is_private: true, visibility: 'limited', follow_state: 'requested' },
	u_blocked: { id: 'u_blocked', handle: 'blocked.user', display_name: 'کاربر مزاحم', followers: 3, following: 900, posts: 2, visibility: 'limited', follow_state: 'none', blocked_by_you: true },
	u_mina: { id: 'u_mina', handle: 'mina', display_name: 'مینا رستمی', verified: true, avatar_id: 'av_mina', meta_line: 'نویسنده و مترجم', province: 'IR-16', bio: 'ترجمه می‌کنم، می‌نویسم و گاهی عکس می‌گیرم 📚\nخبرنامه: https://example.org/mina\nبا @sara و @ali.r در #کتاب می‌نویسیم.', link: 'https://mina.example.org/', followers: 23400, following: 512, posts: 1380, visibility: 'full', follow_state: 'following', follows_you: true, birthday: { calendar: 'gregorian', month: 3, day: 4 }, mutuals: [{ id: 'u_sara', handle: 'sara', display_name: 'سارا احمدی' }, { id: 'u_reza', handle: 'reza.k', display_name: 'رضا کریمی' }, { id: 'u_nika', handle: 'nika', display_name: 'نیکا موسوی' }], mutuals_total: 12 },
	u_gone: { id: 'u_gone', display_name: 'حساب حذف‌شده', deleted: true },
};
const extraHandles = Object.fromEntries(Object.values(extra).filter((u) => u.handle).map((u) => [u.handle, u.id]));
extraHandles.gone = 'u_gone';
// My relationship with each user, once it changed (or was read) through the proxy.
const relation = {};
const muted = new Set(['u_bot']);
let birthdayCalendar = '';
let requests = [
	{ id: 'u_ali', handle: 'ali.r', display_name: 'علی رضایی', is_private: true },
	{ id: 'u_nika', handle: 'nika', display_name: 'نیکا موسوی' },
	{ id: 'u_req3', handle: 'kian_77', display_name: 'کیان نوروزی', verified: true },
];
const crowd = Array.from({ length: 260 }, (_, i) => ({
	id: `u_c${i}`,
	handle: `friend${String(i).padStart(3, '0')}`,
	display_name: ['آرش', 'بهار', 'پویا', 'ترانه', 'داریوش', 'رها', 'سینا', 'شیرین', 'فرهاد', 'گلاره'][i % 10] + ' ' + ['محمدی', 'حسینی', 'کاظمی', 'جعفری', 'نجفی', 'قاسمی', 'اکبری'][i % 7],
	verified: i % 17 === 0,
	is_private: i % 11 === 0,
	follow_state: i % 5 === 0 ? 'following' : i % 13 === 0 ? 'requested' : 'none',
	follows_you: i % 3 === 0,
}));

// The mock's 1x1 PNG does not decode everywhere: avatars get a real picture
// (a 256 px diagonal gradient), encoded here without dependencies.
const crcTable = Array.from({ length: 256 }, (_, n) => {
	let c = n;
	for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
	return c >>> 0;
});
function crc32(buffer) {
	let c = 0xFFFFFFFF;
	for (const byte of buffer) c = crcTable[(c ^ byte) & 0xFF] ^ (c >>> 8);
	return (c ^ 0xFFFFFFFF) >>> 0;
}
function pngChunk(type, data) {
	const head = Buffer.alloc(8);
	head.writeUInt32BE(data.length, 0);
	head.write(type, 4, 'latin1');
	const crc = Buffer.alloc(4);
	crc.writeUInt32BE(crc32(Buffer.concat([head.subarray(4), data])), 0);
	return Buffer.concat([head, data, crc]);
}
function avatarPng(seed) {
	const size = 256;
	const raw = Buffer.alloc(size * (size * 3 + 1));
	const hue = [...seed].reduce((a, ch) => (a * 31 + ch.charCodeAt(0)) % 360, 7);
	const from = [120 + (hue % 100), 90 + ((hue * 3) % 120), 110 + ((hue * 7) % 110)];
	for (let y = 0; y < size; y++) {
		const row = y * (size * 3 + 1);
		for (let x = 0; x < size; x++) {
			const t = (x + y) / (2 * size);
			const ring = Math.hypot(x - size / 2, y - size * 0.42) < size * 0.2 || Math.hypot(x - size / 2, y - size * 1.05) < size * 0.42;
			for (let c = 0; c < 3; c++) raw[row + 1 + x * 3 + c] = ring ? 245 : Math.round(from[c] * (1 - t) + from[(c + 1) % 3] * t);
		}
	}
	const header = Buffer.alloc(13);
	header.writeUInt32BE(size, 0);
	header.writeUInt32BE(size, 4);
	header[8] = 8;
	header[9] = 2;
	return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), pngChunk('IHDR', header), pngChunk('IDAT', zlib.deflateSync(raw)), pngChunk('IEND', Buffer.alloc(0))]);
}

const json = (res, status, body) => {
	res.writeHead(status, { 'Content-Type': status >= 400 ? 'application/problem+json' : 'application/json' });
	res.end(JSON.stringify(body));
};

function upstream(method, path, headers, body) {
	return new Promise((resolve) => {
		const request = http.request({ host: TARGET.hostname, port: TARGET.port, method, path, headers: { ...headers, host: TARGET.host } }, (answer) => {
			const chunks = [];
			answer.on('data', (chunk) => chunks.push(chunk));
			answer.on('end', () => {
				const data = Buffer.concat(chunks);
				let parsed = null;
				try { parsed = JSON.parse(data.toString('utf8')); } catch { /* not json */ }
				resolve({ status: answer.statusCode, headers: answer.headers, data, json: parsed });
			});
		});
		request.on('error', () => resolve({ status: 502, headers: {}, data: Buffer.from(''), json: { code: 'proxy.unreachable', title: 'mock api is not running' } }));
		request.end(body);
	});
}

function pass(res, answer) {
	const headers = { ...answer.headers };
	delete headers['content-length'];
	res.writeHead(answer.status, headers);
	res.end(answer.data);
}

// The profile with the remembered relationship applied.
function withRelation(profile) {
	if (!profile || profile.deleted || profile.id === 'u_me') return profile;
	const known = relation[profile.id] ?? (relation[profile.id] = {
		follow: profile.follow_state || 'none',
		follows_you: !!profile.follows_you,
		blocked_by_you: !!profile.blocked_by_you,
		followers: profile.followers ?? 0,
	});
	const out = { ...profile, follow_state: known.follow, follows_you: known.follows_you, followers: known.followers };
	if (known.blocked_by_you) {
		out.blocked_by_you = true;
		out.visibility = 'limited';
	} else {
		delete out.blocked_by_you;
		out.visibility = profile.is_private && known.follow !== 'following' ? 'limited' : 'full';
	}
	return out;
}

async function profileById(id, headers) {
	if (extra[id]) return withRelation(extra[id]);
	const handle = handles[id];
	if (!handle) return null;
	const answer = await upstream('GET', '/v1/users/by-handle/' + encodeURIComponent(handle), headers);
	return answer.status === 200 ? withRelation(answer.json) : null;
}

function state(id) {
	const known = relation[id];
	return { follow: known?.follow ?? 'none', follows_you: !!known?.follows_you, blocked_by_you: !!known?.blocked_by_you };
}

http.createServer((req, res) => {
	const chunks = [];
	req.on('data', (chunk) => chunks.push(chunk));
	req.on('end', async () => {
		const body = Buffer.concat(chunks);
		const url = new URL(req.url, 'http://x');
		const path = url.pathname;
		const method = req.method;
		const headers = { ...req.headers };
		delete headers['content-length'];
		let parsed = {};
		try { parsed = body.length ? JSON.parse(body.toString('utf8')) : {}; } catch { /* */ }
		let m;

		// ---- uploads: the part goes through the proxy so it can be slow
		if (path === '/v1/uploads' && method === 'POST') {
			const answer = await upstream(method, req.url, headers, body);
			if (answer.json?.parts) {
				answer.json.parts = answer.json.parts.map((part) => ({ ...part, url: `http://127.0.0.1:${PORT}/_put` }));
				return json(res, answer.status, answer.json);
			}
			return pass(res, answer);
		}
		if (path === '/_put') return setTimeout(() => json(res, 200, {}), SLOW_UPLOAD);

		// ---- profiles
		if ((m = path.match(/^\/v1\/users\/by-handle\/([^/]+)$/))) {
			const handle = decodeURIComponent(m[1]).toLowerCase();
			if (extraHandles[handle]) return json(res, 200, withRelation(extra[extraHandles[handle]]));
			const answer = await upstream(method, req.url, headers, body);
			return answer.status === 200 ? json(res, 200, withRelation(answer.json)) : pass(res, answer);
		}
		if ((m = path.match(/^\/v1\/users\/([^/]+)$/)) && method === 'GET') {
			const profile = await profileById(decodeURIComponent(m[1]), headers);
			return profile ? json(res, 200, profile) : json(res, 404, { code: 'user.not_found', title: 'پیدا نشد' });
		}
		if ((m = path.match(/^\/v1\/users\/([^/]+)\/relationship$/))) {
			await profileById(m[1], headers);
			return json(res, 200, state(m[1]));
		}
		if ((m = path.match(/^\/v1\/users\/([^/]+)\/(follow|block)$/))) {
			const id = decodeURIComponent(m[1]);
			const profile = await profileById(id, headers);
			if (!profile) return json(res, 404, { code: 'user.not_found', title: 'پیدا نشد' });
			const known = relation[id];
			if (m[2] === 'follow') {
				const was = known.follow;
				known.follow = method === 'POST' ? (profile.is_private ? 'requested' : 'following') : 'none';
				known.followers += (known.follow === 'following' ? 1 : 0) - (was === 'following' ? 1 : 0);
			} else {
				known.blocked_by_you = method === 'POST';
				if (known.blocked_by_you) {
					known.follow = 'none';
					known.follows_you = false;
				}
			}
			return json(res, 200, state(id));
		}
		if ((m = path.match(/^\/v1\/users\/([^/]+)\/mute$/))) {
			if (method === 'POST') muted.add(m[1]); else muted.delete(m[1]);
			return json(res, 204, {});
		}
		if (path === '/v1/me/filters') {
			const answer = await upstream(method, req.url, headers, body);
			return json(res, 200, { ...(answer.json || {}), muted: [...muted] });
		}
		if ((m = path.match(/^\/v1\/me\/followers\/([^/]+)$/)) && method === 'DELETE') {
			const id = decodeURIComponent(m[1]);
			await profileById(id, headers);
			if (relation[id]) relation[id].follows_you = false;
			return json(res, 200, state(id));
		}
		if ((m = path.match(/^\/v1\/users\/([^/]+)\/conversations\/open$/)) && extra[m[1]]) {
			return json(res, 200, { conversations: [] });
		}
		if ((m = path.match(/^\/v1\/users\/([^/]+)\/avatar$/))) {
			res.writeHead(200, { 'Content-Type': 'image/png' });
			return res.end(avatarPng(m[1]));
		}

		// ---- follow lists
		if ((m = path.match(/^\/v1\/users\/([^/]+)\/(followers|following)$/))) {
			if (m[1] !== 'u_sara') return pass(res, await upstream(method, req.url, headers, body));
			const page = Number((url.searchParams.get('cursor') || 'p0').slice(1));
			const size = 100;
			const users = crowd.slice(page * size, (page + 1) * size);
			const out = { users };
			if (page === 0) {
				const first = await upstream(method, path, headers);
				out.users = [...(first.json?.users || []).map(withRelation), ...users];
			}
			if ((page + 1) * size < crowd.length) out.next_cursor = 'p' + (page + 1);
			return setTimeout(() => json(res, 200, out), page ? 400 : 0);
		}
		if (path === '/v1/me/follow-requests') return json(res, 200, { users: requests });
		if ((m = path.match(/^\/v1\/me\/follow-requests\/([^/]+)\/(accept|decline)$/))) {
			const id = decodeURIComponent(m[1]);
			requests = requests.filter((u) => u.id !== id);
			return json(res, 200, { follow: 'none', follows_you: m[2] === 'accept', blocked_by_you: false });
		}
		if (path === '/v1/notifications/unread') {
			const answer = await upstream(method, req.url, headers, body);
			return json(res, 200, { ...(answer.json || {}), kinds: { follow_request: requests.length } });
		}

		// ---- posts
		if ((m = path.match(/^\/v1\/users\/([^/]+)\/posts$/))) {
			if (extra[m[1]]) return json(res, 200, { posts: [] });
			if (url.searchParams.get('tab') === 'replies') return json(res, 200, { posts: [] });
			if (m[1] === 'u_nika') {
				const page = Number((url.searchParams.get('before') || 'b0').slice(1));
				const posts = Array.from({ length: 20 }, (_, i) => {
					const n = page * 20 + i + 1;
					return {
						id: `pn${n}`, author: 'u_nika', type: n % 6 === 0 ? 'media' : 'text', created_at: Date.now() - n * 3600_000,
						text: `یادداشت شمارهٔ ${n}: ` + 'امروز دوباره به این فکر کردم که نوشتن کوتاه و منظم چقدر حال آدم را بهتر می‌کند. '.repeat(1 + (n % 3)),
						media: n % 6 === 0 ? [{ id: `mn${n}`, kind: 'image', width: 800, height: 600 }] : undefined,
						reply_policy: 'everyone', can_reply: true, can_repost: true,
						counts: { likes: n * 3, dislikes: 0, replies: n % 5, reposts: 0, quotes: 0, views: n * 40 }, my: {},
					};
				});
				const out = { posts };
				if (page < 2) out.next_before = 'b' + (page + 1);
				return setTimeout(() => json(res, 200, out), page ? 300 : 0);
			}
		}
		if (path === '/v1/me/bookmarks') {
			if (url.searchParams.get('cursor')) return json(res, 200, { posts: [] });
			const answer = await upstream('GET', '/v1/users/u_sara/posts', headers);
			const posts = (answer.json?.posts || []).slice(0, 2).map((post) => ({ ...post, my: { ...post.my, bookmarked: true } }));
			return json(res, 200, { posts, authors: answer.json?.authors });
		}

		// ---- editing my profile
		if ((m = path.match(/^\/v1\/handles\/([^/]+)\/availability$/))) {
			const handle = decodeURIComponent(m[1]);
			if (handle.startsWith('fail')) return json(res, 500, { code: 'internal', title: 'خطای سرور' });
			if (handle.startsWith('taken')) return json(res, 200, { available: false });
			const answer = await upstream(method, req.url, headers, body);
			return setTimeout(() => pass(res, answer), handle.startsWith('slow') ? 2000 : 0);
		}
		if (path === '/v1/me/handle' && method === 'PUT' && parsed.handle === 'reserved') {
			return json(res, 400, { code: 'handle.reserved_for_bots', title: 'شناسه‌هایی که به bot ختم می‌شوند فقط برای ربات‌ها هستند' });
		}
		if (path === '/v1/me/handle' && method === 'PUT' && String(parsed.handle).startsWith('fail')) {
			return json(res, 409, { code: 'handle.taken', title: 'این شناسه قبلاً گرفته شده است' });
		}
		if (path === '/v1/me/birthday' && method === 'PUT' && parsed.year === 1300) {
			return json(res, 409, { code: 'birthday.change_limit', title: 'تاریخ تولد را امسال بیش از حد مجاز تغییر داده‌اید' });
		}
		if (path === '/v1/me/birthday' && method === 'PUT') {
			// The mock stores every birthday as Jalali: keep the calendar that was sent.
			const answer = await upstream(method, req.url, headers, body);
			birthdayCalendar = parsed.calendar || 'jalali';
			if (answer.json?.birthday) answer.json.birthday.calendar = birthdayCalendar;
			return json(res, answer.status, answer.json);
		}
		if (path === '/v1/me' && method === 'GET' && birthdayCalendar) {
			const answer = await upstream(method, req.url, headers, body);
			if (answer.json?.birthday) answer.json.birthday.calendar = birthdayCalendar;
			return json(res, answer.status, answer.json);
		}
		pass(res, await upstream(method, req.url, headers, body));
	});
}).listen(PORT, '127.0.0.1', () => {
	console.log(`profile proxy on http://127.0.0.1:${PORT} -> ${TARGET.href}`);
});
