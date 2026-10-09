#!/usr/bin/env node
// A thin proxy in front of the web client's mock API (pwa/scripts/mock-api.mjs)
// that adds what the conversation and search screens need and the mock lacks:
// paging, errors, follow answers, video posters, scripted conversation states.
//
//   PORT=8000 node <pwa>/scripts/mock-api.mjs &
//   PORT=8010 TARGET=http://127.0.0.1:8000 node tests/tools/mock-social-proxy.mjs &
//   Eleza --api=http://127.0.0.1:8010 ...
//
// Conversations
//   GET  /v1/conversations/open?category=tech    45 generated topics, 20 a page (cursor)
//   GET  /v1/conversations/open?category=sport   an empty list
//   GET  /v1/conversations/open?category=books   500
//   GET  /v1/conversations/radar_waiting         my topic, nobody listens yet
//   GET  /v1/conversations/radar_named           the same, opened under my name
//   GET  /v1/conversations/radar_matched         my topic with a (masked) listener
//   GET  /v1/conversations/radar_friend          opened under my name; after two reads u_sara listens
//   GET  /v1/conversations/radar_flow            waiting for the first two reads, then matched
//   GET  /v1/conversations/radar_ending          waiting for the first two reads, then ended
//   GET  /v1/conversations/radar_broken          500
//   POST /v1/conversations  topic with «سقف» / "limit" -> 409 conversation.limit_reached
//                           topic with «زیاد» / "rate"  -> 429
//   POST /v1/conversations/conv4/join            409 conversation.not_open
//   POST /v1/conversations/conv2/join?           (the mock's own answer)
// Search (GET /v1/search)
//   q with "error"                 500;  q with "ratelimit" -> 429
//   q «کاربر» / "user", type=users  45 people, 20 a page (next_offset), mixed follow states
//   q «نوشته» / "post", type=posts  30 posts, 10 a page
//   q «موضوع» / "topic", type=topics 8 slugs
//   q «گفتگو» / "talk", type=conversations  6 open topics
// Discover
//   GET  /v1/feed/video             9 video posts with posters
//   GET  /v1/posts/v*/media/*       a generated poster (PNG)
//   GET  /v1/suggestions/people     12 people, 6 a page; dismissed ones never come back
//   GET  /v1/topics/trending?       8 trends when started with TRENDS=many
// Graph
//   POST / DELETE /v1/users/<id>/follow   the new relationship (private accounts -> requested;
//                                         u_fail -> 500)
// Start with BROWSE=fail to make the three browse requests fail, BROWSE=empty for empty ones,
// SLOW=<ms> to delay the answers of the open list, the search and the browse requests (skeletons).
import http from 'node:http';
import zlib from 'node:zlib';

const PORT = Number(process.env.PORT || 8010);
const TARGET = new URL(process.env.TARGET || 'http://127.0.0.1:8000');
const BROWSE = process.env.BROWSE || '';
const TRENDS = process.env.TRENDS || '';
const SLOW = Number(process.env.SLOW || 0);
const slowPaths = ['/v1/conversations/open', '/v1/search', '/v1/feed/video', '/v1/topics/trending', '/v1/suggestions/people'];

const json = (res, status, body) => {
	const text = JSON.stringify(body);
	res.writeHead(status, { 'Content-Type': status >= 400 ? 'application/problem+json' : 'application/json' });
	res.end(text);
};

function forward(req, res, body) {
	const options = {
		host: TARGET.hostname,
		port: TARGET.port,
		method: req.method,
		path: req.url,
		headers: { ...req.headers, host: TARGET.host },
	};
	const upstream = http.request(options, (answer) => {
		res.writeHead(answer.statusCode, answer.headers);
		answer.pipe(res);
	});
	upstream.on('error', () => json(res, 502, { code: 'proxy.unreachable', title: 'mock api is not running' }));
	upstream.end(body);
}

// ---- a tiny PNG encoder (posters)
const crcTable = (() => {
	const table = new Uint32Array(256);
	for (let n = 0; n < 256; n++) {
		let c = n;
		for (let k = 0; k < 8; k++) c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
		table[n] = c >>> 0;
	}
	return table;
})();
function crc32(buffer) {
	let c = 0xffffffff;
	for (const byte of buffer) c = crcTable[(c ^ byte) & 0xff] ^ (c >>> 8);
	return (c ^ 0xffffffff) >>> 0;
}
function chunk(type, data) {
	const head = Buffer.alloc(8);
	head.writeUInt32BE(data.length, 0);
	head.write(type, 4, 'ascii');
	const tail = Buffer.alloc(4);
	tail.writeUInt32BE(crc32(Buffer.concat([head.subarray(4), data])), 0);
	return Buffer.concat([head, data, tail]);
}
const palettes = [
	[[0x06, 0x41, 0x2c], [0x7f, 0xdd, 0xa5]], [[0xe0, 0x9a, 0x2b], [0xd6, 0x4f, 0x45]], [[0x5c, 0x8f, 0xd6], [0x8e, 0x7b, 0xd0]],
	[[0x3a, 0xa3, 0xa8], [0xfc, 0xef, 0xd6]], [[0xc9, 0x73, 0xa6], [0x24, 0x28, 0x24]], [[0x8f, 0xa8, 0x4a], [0x06, 0x41, 0x2c]],
];
function poster(seed) {
	const width = 180;
	const height = 320;
	const [a, b] = palettes[seed % palettes.length];
	const raw = Buffer.alloc((width * 3 + 1) * height);
	for (let y = 0; y < height; y++) {
		const row = y * (width * 3 + 1);
		raw[row] = 0;
		for (let x = 0; x < width; x++) {
			const t = (x / width) * 0.35 + (y / height) * 0.65;
			const dx = x - width * (0.3 + 0.1 * (seed % 4));
			const dy = y - height * 0.45;
			const spot = Math.max(0, 1 - Math.hypot(dx, dy) / 70);
			for (let i = 0; i < 3; i++) {
				const value = a[i] + (b[i] - a[i]) * t;
				raw[row + 1 + x * 3 + i] = Math.min(255, Math.round(value + (255 - value) * spot * 0.55));
			}
		}
	}
	const header = Buffer.alloc(13);
	header.writeUInt32BE(width, 0);
	header.writeUInt32BE(height, 4);
	header[8] = 8;
	header[9] = 2;
	return Buffer.concat([
		Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
		chunk('IHDR', header),
		chunk('IDAT', zlib.deflateSync(raw)),
		chunk('IEND', Buffer.alloc(0)),
	]);
}

// ---- fixtures
const nowSec = () => Math.floor(Date.now() / 1000);
const firstNames = ['آرش', 'مینا', 'کاوه', 'نگار', 'پویا', 'ترانه', 'سینا', 'هستی', 'بابک', 'لیلا', 'امید', 'ژیلا'];
const lastNames = ['کاربر', 'کاربری', 'کاربرزاده'];
const people = Array.from({ length: 45 }, (_, i) => {
	const state = i % 7 === 2 ? 'following' : i % 11 === 5 ? 'requested' : 'none';
	return {
		id: 'u_gen' + i,
		handle: 'user' + (i + 1),
		display_name: `${firstNames[i % firstNames.length]} ${lastNames[i % lastNames.length]}`,
		verified: i % 6 === 0,
		is_private: i % 5 === 3,
		follow_state: state,
		follows_you: i % 4 === 1,
		bio: i % 3 === 0 ? 'اینجا از کتاب، کوه و قهوه می‌نویسم؛ گاهی هم از کار.' : undefined,
	};
});
const peopleById = Object.fromEntries(people.map((p) => [p.id, p]));
const dismissed = new Set();
const relations = {};
const suggestionPool = [
	{ id: 'u_nika', handle: 'nika', display_name: 'نیکا موسوی', follow_state: 'none', reason: 'mutual', mutual_count: 3, mutuals: [{ id: 'u_sara', display_name: 'سارا احمدی' }, { id: 'u_ali', display_name: 'علی رضایی' }] },
	{ id: 'u_reza', handle: 'reza.k', display_name: 'رضا کریمی', verified: true, follow_state: 'none', reason: 'popular' },
	{ id: 'u_sara', handle: 'sara', display_name: 'سارا احمدی', verified: true, follow_state: 'none', follows_you: true, reason: 'follows_you' },
	{ id: 'u_ali', handle: 'ali.r', display_name: 'علی رضایی', is_private: true, follow_state: 'none', reason: 'province' },
	{ ...people[0], follow_state: 'none', reason: 'contact' },
	{ ...people[1], follow_state: 'none', reason: 'interest' },
	{ ...people[4], follow_state: 'none', reason: 'mutual', mutual_count: 1, mutuals: [{ id: 'u_sara', display_name: 'سارا احمدی' }] },
	{ ...people[6], follow_state: 'none', reason: 'mutual', mutual_count: 12 },
	{ ...people[7], follow_state: 'none', reason: 'active' },
	{ ...people[8], follow_state: 'none', reason: 'mutual', mutual_count: 2, mutuals: [{ id: 'u_sara', display_name: 'سارا احمدی' }, { id: 'u_nika', display_name: 'نیکا موسوی' }] },
	{ ...people[9], follow_state: 'none', reason: 'province' },
	{ ...people[10], follow_state: 'none', reason: 'popular' },
];
const videoPosts = Array.from({ length: 9 }, (_, i) => ({
	id: 'v' + (i + 1),
	author: i % 2 ? 'u_sara' : 'u_ali',
	type: 'media',
	text: 'ویدیوی شماره ' + (i + 1),
	created_at: Date.now() - (i + 1) * 3600_000,
	media: [{ id: 'vm' + (i + 1), kind: 'video', width: 720, height: 1280, duration_ms: 12_000 + i * 1000 }],
	counts: { likes: 10 * (i + 1), replies: i, reposts: 0, quotes: 0, views: 1000 * (i + 1) },
	my: {},
	can_reply: true,
	can_repost: true,
}));
const authors = {
	u_sara: { id: 'u_sara', handle: 'sara', display_name: 'سارا احمدی', verified: true },
	u_ali: { id: 'u_ali', handle: 'ali.r', display_name: 'علی رضایی' },
};
const textPosts = Array.from({ length: 30 }, (_, i) => ({
	id: 'gp' + (i + 1),
	author: i % 2 ? 'u_sara' : 'u_ali',
	type: 'text',
	text: `نوشته شماره ${i + 1}: امروز دوباره به این فکر می‌کردم که چقدر حرف نزده داریم و چقدر شنیدن آدم‌ها آرامم می‌کند. شاید یک گفتگوی تازه باز کنم.`,
	created_at: Date.now() - (i + 1) * 5400_000,
	counts: { likes: 3 * i, replies: i % 4, reposts: 0, quotes: 0, views: 100 * i },
	my: {},
	can_reply: true,
	can_repost: true,
}));
const topicsMany = ['موضوع', 'موضوع_روز', 'موضوع_آزاد', 'موضوع_کتاب', 'موضوع_فیلم', 'موضوع_سفر', 'موضوع_کار', 'موضوع_شهر'];
const trendsMany = Array.from({ length: 8 }, (_, i) => ({ id: 'tm' + i, slug: ['تهران', 'الزا', 'کتاب', 'سینما', 'فوتبال', 'موسیقی', 'سفر', 'قهوه'][i], title: ['تهران', 'الزا', 'کتاب', 'سینما', 'فوتبال', 'موسیقی', 'سفر', 'قهوه'][i], posts: 1240 - i * 130, authors: 530 - i * 60, open_conversations: (8 - i) % 5, rising: i < 2 }));
const techTopics = Array.from({ length: 45 }, (_, i) => ({
	id: 'tech' + i,
	topic: ['کسی با Rust کار جدی کرده؟ از کجا شروع کنم', 'لپ‌تاپ برای برنامه‌نویسی، مک یا لینوکس؟', 'هوش مصنوعی شغل ما را می‌گیرد؟', 'اولین مصاحبهٔ کاری‌ام فرداست و می‌ترسم', 'دورکاری بعد از سه سال: خوبی‌ها و بدی‌هایش'][i % 5] + ` (${i + 1})`,
	category: 'tech',
	expires_at: nowSec() + 600 + i * 1700,
	speaker_masked: i % 3 !== 0,
	mask_name: i % 3 === 1 ? 'رهگذر' : undefined,
	speaker_id: i % 3 === 0 ? (i % 2 ? 'u_sara' : 'u_ali') : undefined,
}));
const talkTopics = Array.from({ length: 6 }, (_, i) => ({
	id: 'talk' + i,
	topic: `گفتگو دربارهٔ ${['کار', 'خانواده', 'مهاجرت', 'تنهایی', 'کتاب', 'سفر'][i]} — کسی هست بشنود؟`,
	category: ['study_work', 'family', 'migration', 'heart', 'books', 'other'][i],
	expires_at: nowSec() + 1800 + i * 4000,
	masked: true,
	mask_name: 'رهگذر',
}));

// The scripted flows start over when nobody read the conversation for a while
// (a new run of the app).
const radarReads = {};
const radarSeen = {};
function radarView(id) {
	if (Date.now() - (radarSeen[id] || 0) > 10_000) radarReads[id] = 0;
	radarSeen[id] = Date.now();
	const base = {
		id,
		topic: 'امروز دلم می‌خواهد از شهری که ترکش کردم حرف بزنم',
		category: 'migration',
		state: 'open',
		expires_at: nowSec() + 4 * 3600 + 59 * 60,
		speaker: { ref: 'p_me', me: true, role: 'speaker', masked: true, mask_name: 'رهگذر' },
		my_role: 'speaker',
	};
	const masked = { ref: 'p_l', masked: true, mask_name: 'رهگذر', role: 'listener' };
	const reads = radarReads[id] = (radarReads[id] || 0) + 1;
	switch (id) {
	case 'radar_waiting': return base;
	case 'radar_named': return { ...base, speaker: { ref: 'u_me', me: true, role: 'speaker', user_id: 'u_me' } };
	case 'radar_matched': return { ...base, state: 'active', listener: masked };
	case 'radar_friend': {
		const named = { ...base, speaker: { ref: 'u_me', me: true, role: 'speaker', user_id: 'u_me' } };
		return reads > 2 ? { ...named, state: 'active', listener: { ref: 'u_sara', user_id: 'u_sara', role: 'listener' } } : named;
	}
	case 'radar_flow': return reads > 2 ? { ...base, state: 'active', listener: masked } : base;
	case 'radar_ending': return reads > 2 ? { ...base, state: 'ended', end_reason: 'speaker' } : base;
	}
	return null;
}

function relationOf(id) {
	const person = peopleById[id];
	return relations[id] || { follow: person?.follow_state || 'none', follows_you: !!person?.follows_you, blocked_by_you: false };
}

http.createServer((req, res) => {
	const chunks = [];
	req.on('data', (part) => chunks.push(part));
	req.on('end', () => {
		const delay = (SLOW > 0 && slowPaths.includes(new URL(req.url, 'http://x').pathname)) ? SLOW : 0;
		setTimeout(() => handle(req, res, Buffer.concat(chunks)), delay);
	});
}).listen(PORT, '127.0.0.1', () => {
	console.log(`mock social proxy on http://127.0.0.1:${PORT} -> ${TARGET.origin}`);
});

function handle(req, res, body) {
	{
		const url = new URL(req.url, 'http://x');
		const path = url.pathname;
		const method = req.method;
		let parsed = {};
		try {
			parsed = body.length ? JSON.parse(body.toString('utf8')) : {};
		} catch { /* not json */ }
		let match;

		// ---- conversations
		if (path === '/v1/conversations/open') {
			const category = url.searchParams.get('category');
			if (category === 'tech') {
				const from = Number(url.searchParams.get('cursor') || 0);
				const page = techTopics.slice(from, from + 20);
				return json(res, 200, { conversations: page, next_cursor: from + 20 < techTopics.length ? String(from + 20) : undefined });
			} else if (category === 'sport') {
				return json(res, 200, { conversations: [] });
			} else if (category === 'books') {
				return json(res, 500, { code: 'internal', title: 'خطای داخلی' });
			}
		}
		if (method === 'GET' && (match = path.match(/^\/v1\/conversations\/(radar_[a-z]+)$/))) {
			if (match[1] === 'radar_broken') return json(res, 500, { code: 'internal', title: 'خطای داخلی' });
			const view = radarView(match[1]);
			if (view) return json(res, 200, view);
		}
		if (method === 'GET' && (match = path.match(/^\/v1\/conversations\/(tech\d+|talk\d+)$/))) {
			const item = techTopics.find((t) => t.id === match[1]) || talkTopics.find((t) => t.id === match[1]);
			if (item) {
				return json(res, 200, {
					id: item.id, topic: item.topic, category: item.category, state: 'open', expires_at: item.expires_at,
					speaker: { ref: item.speaker_id || 'p_s', masked: item.speaker_masked ?? true, mask_name: item.mask_name, user_id: item.speaker_id, role: 'speaker' },
				});
			}
		}
		if (method === 'POST' && (match = path.match(/^\/v1\/conversations\/(radar_[a-z]+)\/end$/))) return json(res, 204, {});
		if (method === 'POST' && path === '/v1/conversations') {
			const topic = String(parsed.topic || '');
			if (topic.includes('سقف') || topic.includes('limit')) return json(res, 409, { code: 'conversation.limit_reached', title: 'به سقف گفت‌وگوهای هم‌زمان رسیده‌اید' });
			if (topic.includes('زیاد') || topic.includes('rate')) return json(res, 429, { code: 'rate_limited', title: 'درخواست‌های زیاد' });
		}
		if (method === 'POST' && path === '/v1/conversations/conv4/join') return json(res, 409, { code: 'conversation.not_open', title: 'موضوع باز نیست' });
		if (method === 'POST' && (match = path.match(/^\/v1\/conversations\/(tech\d+|talk\d+)\/join$/))) return json(res, 403, { code: 'conversation.join_not_allowed', title: 'امکان پیوستن به این گفت‌وگو وجود ندارد' });

		// ---- search
		if (path === '/v1/search') {
			const q = (url.searchParams.get('q') || '').toLowerCase();
			const type = url.searchParams.get('type') || 'users';
			const offset = Number(url.searchParams.get('offset') || 0);
			if (q.includes('error')) return json(res, 500, { code: 'internal', title: 'خطای داخلی' });
			if (q.includes('ratelimit')) return json(res, 429, { code: 'rate_limited', title: 'درخواست‌های زیاد' });
			if (q.includes('کاربر') || q.includes('user')) {
				if (type === 'users') {
					const page = people.slice(offset, offset + 20).map((p) => ({ ...p, follow_state: relationOf(p.id).follow, follows_you: relationOf(p.id).follows_you }));
					return json(res, 200, { users: page, next_offset: offset + 20 < people.length ? offset + 20 : undefined });
				}
				return json(res, 200, {});
			}
			if (q.includes('نوشته') || q.includes('post')) {
				if (type === 'posts') {
					const page = textPosts.slice(offset, offset + 10);
					return json(res, 200, { posts: page, authors, next_offset: offset + 10 < textPosts.length ? offset + 10 : undefined });
				}
				return json(res, 200, {});
			}
			if (q.includes('موضوع') || q.includes('topic')) {
				return json(res, 200, type === 'topics' ? { topics: offset ? [] : topicsMany } : {});
			}
			if (q.includes('گفتگو') || q.includes('talk')) {
				return json(res, 200, type === 'conversations' ? { conversations: offset ? [] : talkTopics.map((t) => ({ ...t, expires_at: Math.max(t.expires_at, nowSec() + 600) })) } : {});
			}
		}

		// ---- discover
		if (BROWSE === 'fail' && (path === '/v1/feed/video' || path === '/v1/topics/trending' || path === '/v1/suggestions/people')) {
			return json(res, 500, { code: 'internal', title: 'خطای داخلی' });
		}
		if (BROWSE === 'empty') {
			if (path === '/v1/feed/video') return json(res, 200, { items: [], authors: {} });
			if (path === '/v1/topics/trending') return json(res, 200, { topics: [] });
			if (path === '/v1/suggestions/people') return json(res, 200, { people: [] });
		}
		if (path === '/v1/topics/trending' && TRENDS === 'many') return json(res, 200, { topics: trendsMany });
		if (path === '/v1/feed/video') {
			if (url.searchParams.get('cursor')) return json(res, 200, { items: [], authors: {} });
			return json(res, 200, { items: videoPosts.map((post) => ({ post })), authors });
		}
		if ((match = path.match(/^\/v1\/posts\/v(\d+)\/media\/[^/]+$/))) {
			res.writeHead(200, { 'Content-Type': 'image/png' });
			return res.end(poster(Number(match[1])));
		}
		if (path === '/v1/suggestions/people') {
			const pool = suggestionPool.filter((p) => !dismissed.has(p.id));
			const from = Number(url.searchParams.get('cursor') || 0);
			const page = pool.slice(from, from + 6);
			return json(res, 200, { people: page, next_cursor: from + 6 < pool.length ? String(from + 6) : undefined });
		}
		if (method === 'POST' && (match = path.match(/^\/v1\/suggestions\/people\/([^/]+)\/dismiss$/))) {
			dismissed.add(decodeURIComponent(match[1]));
			return json(res, 204, {});
		}

		// ---- graph
		if ((match = path.match(/^\/v1\/users\/([^/]+)\/follow$/))) {
			const id = decodeURIComponent(match[1]);
			if (id === 'u_fail' || id === 'u_gen13') return json(res, 500, { code: 'internal', title: 'خطای داخلی' });
			const isPrivate = !!peopleById[id]?.is_private || id === 'u_ali';
			const previous = relationOf(id);
			relations[id] = {
				follow: method === 'DELETE' ? 'none' : (isPrivate ? 'requested' : 'following'),
				follows_you: previous.follows_you || id === 'u_sara',
				blocked_by_you: false,
			};
			return json(res, 200, relations[id]);
		}

		forward(req, res, body);
	}
}
