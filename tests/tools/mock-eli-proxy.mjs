#!/usr/bin/env node
// A proxy in front of the web client's mock API (pwa/scripts/mock-api.mjs) that
// gives the «الی» screens what the mock lacks:
//
//   * real media: every post picture / poster is a generated image, every video
//     variant a short generated mp4, every audio variant a short generated m4a
//     (ffmpeg builds them once into a temporary directory);
//   * the post types the mock has no fixture for: two / three image albums, a
//     video with a sound, a round video note, an audio clip, a long text (the
//     «نمایش بیشتر» clamp), a closed poll I voted in, a quote of a post with media,
//     a quote of an unavailable post, a deleted post;
//   * a video feed (/v1/feed/video) and a sound page (/v1/sounds/s1);
//   * a deep replies tree with two pages under p1 (folding and pagination);
//   * «N new posts»: GET /v1/feed/following/new answers NEW_POSTS (env, default 0);
//   * notifications on top of the mock's: a reply to my comment on page 2 of p1's
//     replies (refs.reply / parent / root, the post opens at it), a mention in a
//     comment, and a grouped follow of five (three listed, one I follow already).
//
//   PORT=8104 node <pwa>/scripts/mock-api.mjs &
//   PORT=8144 TARGET=http://127.0.0.1:8104 node tests/tools/mock-eli-proxy.mjs &
//   Eleza --api=http://127.0.0.1:8144 ...
import http from 'node:http';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';

const PORT = Number(process.env.PORT || 8144);
const TARGET = new URL(process.env.TARGET || 'http://127.0.0.1:8104');
const NEW_POSTS = Number(process.env.NEW_POSTS || 0);
const CACHE = path.join(os.tmpdir(), 'eleza-mock-eli-media');
fs.mkdirSync(CACHE, { recursive: true });

const now = Date.now();
const counts = (likes, replies = 0, reposts = 0, views = 0, dislikes = 0) => ({ likes, dislikes, replies, reposts, quotes: 0, views });
const base = (id, author, type, extra = {}, at = now) => ({ id, author, type, created_at: at, reply_policy: 'everyone', can_reply: true, can_repost: true, counts: counts(0), my: {}, ...extra });

const users = {
	u_sara: { id: 'u_sara', handle: 'sara', display_name: 'سارا احمدی', verified: true },
	u_ali: { id: 'u_ali', handle: 'ali.r', display_name: 'علی رضایی' },
	u_nika: { id: 'u_nika', handle: 'nika', display_name: 'نیکا موسوی' },
	u_reza: { id: 'u_reza', handle: 'reza.k', display_name: 'رضا کریمی', verified: true },
};

const sound = { id: 's1', kind: 'video', owner: 'u_reza', usable: true, uses: 2, title: 'باران پاییزی', artist: 'رضا کریمی', origin_post: 'px_video', media: 'v1', duration_ms: 6000, mine: false, reuse_allowed: true };
const longText = [
	'دیروز عصر توی یه کافهٔ کوچیک نشسته بودم و به این فکر می‌کردم که چقدر عادت کردیم همه‌چیز رو سریع بخونیم و رد بشیم.',
	'یه نفر کنار پنجره داشت کتاب می‌خوند؛ نیم ساعت تمام حتی یک بار هم به گوشی‌اش نگاه نکرد.',
	'همون لحظه تصمیم گرفتم یک هفته امتحان کنم: هر روز فقط یک متن بلند، با حوصله، از اول تا آخر.',
	'روز اول سخت بود. ذهنم مدام دنبال چیز تازه می‌گشت و هر دو خط یک بار حواسم پرت می‌شد.',
	'روز سوم فهمیدم مشکل از متن‌ها نیست؛ من یادم رفته بود چطور منتظر بمونم تا یک فکر کامل بشه.',
	'حالا یک هفته گذشته. هنوز هم گاهی وسط خوندن دستم می‌ره سمت گوشی، ولی کمتر.',
	'اگه شما هم امتحان کردید بنویسید چطور بود. #کتاب #عادت @sara',
].join('\n');

const extra = {};
const add = (post) => (extra[post.id] = post);
add(base('px_two', 'u_nika', 'media', { text: 'دو قاب از بازار تجریش', media: [
	{ id: 'i21', kind: 'image', width: 900, height: 1200, blurhash: 'LEHV6nWB2yk8pyo0adR*.7kCMdnj', alt: 'میوه‌فروشی' },
	{ id: 'i22', kind: 'image', width: 900, height: 1200, blurhash: 'LGF5]+Yk^6#M@-5c,1J5@[or[Q6.' }], counts: counts(31, 2, 1, 640) }, now - 50 * 60_000));
add(base('px_three', 'u_reza', 'media', { text: 'سه‌تایی از جادهٔ چالوس', media: [
	{ id: 'i31', kind: 'image', width: 1000, height: 1400, blurhash: 'L6PZfSi_.AyE_3t7t7R**0o#DgR4' },
	{ id: 'i32', kind: 'image', width: 1200, height: 800, blurhash: 'LKO2?U%2Tw=w]~RBVZRi};RPxuwH' },
	{ id: 'i33', kind: 'image', width: 1200, height: 800, blurhash: 'LEHV6nWB2yk8pyo0adR*.7kCMdnj' }], counts: counts(204, 14, 22, 8800, 3) }, now - 70 * 60_000));
add(base('px_video', 'u_reza', 'media', { text: 'باران امروز تهران 🌧 #تهران', media: [
	{ id: 'v1', kind: 'video', width: 540, height: 960, duration_ms: 6000, blurhash: 'LGF5]+Yk^6#M@-5c,1J5@[or[Q6.' }], sound, counts: counts(1890, 46, 120, 45200, 12), my: {} }, now - 95 * 60_000));
add(base('px_video2', 'u_nika', 'media', { text: 'غروب از پشت‌بام', media: [
	{ id: 'v2', kind: 'video', width: 960, height: 540, duration_ms: 5000, blurhash: 'LKO2?U%2Tw=w]~RBVZRi};RPxuwH' }], sound: { ...sound, origin_post: 'px_video' }, counts: counts(312, 9, 14, 7300), my: { reaction: 'like' } }, now - 4 * 3600_000));
add(base('px_note', 'u_sara', 'video_note', { text: 'سلام از بالای کوه!', media: [
	{ id: 'n1', kind: 'video_note', width: 480, height: 480, duration_ms: 5000, blurhash: 'L6PZfSi_.AyE_3t7t7R**0o#DgR4' }], counts: counts(58, 3, 0, 900) }, now - 110 * 60_000));
add(base('px_audio', 'u_ali', 'audio', { text: 'این آهنگ رو امروز ده بار گوش دادم', media: [
	{ id: 'a1', kind: 'audio_clip', duration_ms: 34000 }], clip: { media: 'a1', source: 'device', title: 'باران', artist: 'گروه پالت', start_ms: 0, end_ms: 34000 }, counts: counts(77, 5, 6, 1500) }, now - 130 * 60_000));
add(base('px_long', 'u_nika', 'text', { text: longText, counts: counts(412, 37, 58, 16400, 4), hashtags: ['کتاب', 'عادت'] }, now - 150 * 60_000));
add(base('px_closed', 'u_reza', 'poll', { text: 'بهترین فصل برای سفر؟', poll: { question: 'بهترین فصل برای سفر؟', options: ['بهار', 'تابستان', 'پاییز', 'زمستان'], ends_at: Math.floor(now / 1000) - 3600, closed: true, votes: [41, 9, 63, 7], total: 120, my_vote: 2 }, counts: counts(19, 4, 1, 2600) }, now - 26 * 3600_000));
add(base('px_qmedia', 'u_sara', 'quote', { text: 'این قاب رو ببینید 😍', quote_of: 'p2', counts: counts(12, 1, 0, 300) }, now - 170 * 60_000));
add(base('px_qgone', 'u_ali', 'quote', { text: 'حیف که پاکش کرد…', quote_unavailable: true, counts: counts(2, 0, 0, 80) }, now - 190 * 60_000));
add(base('px_deleted', 'u_nika', 'text', { deleted: true, counts: counts(0) }, now - 200 * 60_000));
add(base('px_closedreplies', 'u_reza', 'text', { text: 'پاسخ‌های این پست بسته است؛ فقط برای خواندن.', can_reply: false, reply_policy: 'mentioned', counts: counts(9, 0, 1, 410) }, now - 210 * 60_000));

const moreReplies = [];
for (let i = 4; i <= 9; i++) {
	moreReplies.push(base('r' + i, i % 2 ? 'u_nika' : 'u_reza', 'text', { text: ['من هم میام!', 'کجا جمع می‌شیم؟', 'ساعت چند؟', 'هوا رو چک کردین؟ می‌گن بارونیه ☔', 'من ماشین میارم', 'عالیه، پس جمعه ۷ صبح'][i - 4], reply_to: i === 4 ? 'r1' : 'r' + (i - 1 === 4 ? 4 : 1), thread_root: 'p1', counts: counts(i) }, now - (14 - i) * 60_000));
}
const pageTwo = [
	base('r20', 'u_reza', 'text', { text: 'عکس‌هاش رو هم بذار!', reply_to: 'p1', thread_root: 'p1', counts: counts(3) }, now - 5 * 60_000),
	base('r21', 'u_nika', 'text', { text: 'دلم برای کوه تنگ شده 🏔', reply_to: 'p1', thread_root: 'p1', counts: counts(1) }, now - 4 * 60_000),
	base('r22', 'u_sara', 'text', { text: 'می‌ذارم حتماً', reply_to: 'r20', thread_root: 'p1', counts: counts(0) }, now - 3 * 60_000),
];
const extraReplies = Object.fromEntries([...moreReplies, ...pageTwo].map((r) => [r.id, r]));

const feedExtras = () => ['px_two', 'px_three', 'px_video', 'px_note', 'px_audio', 'px_long', 'px_closed', 'px_qmedia', 'px_qgone', 'px_deleted', 'px_closedreplies'].map((id) => ({ post: extra[id] }));

const json = (res, status, body) => {
	const text = JSON.stringify(body);
	res.writeHead(status, { 'Content-Type': status >= 400 ? 'application/problem+json' : 'application/json' });
	res.end(text);
};

function upstream(req, body, done) {
	const request = http.request({
		host: TARGET.hostname,
		port: TARGET.port,
		method: req.method,
		path: req.url,
		headers: { ...req.headers, host: TARGET.host },
	}, (answer) => {
		const chunks = [];
		answer.on('data', (chunk) => chunks.push(chunk));
		answer.on('end', () => done(answer, Buffer.concat(chunks)));
	});
	request.on('error', () => done(null, null));
	request.end(body);
}

function forward(req, res, body, rewrite) {
	upstream(req, body, (answer, data) => {
		if (!answer) {
			return json(res, 502, { code: 'proxy.unreachable', title: 'mock api is not running' });
		}
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
}

// ---- generated media ---------------------------------------------------------------------------

const hash = (text) => [...text].reduce((value, ch) => (value * 31 + ch.charCodeAt(0)) >>> 0, 7);
const palettes = [
	['0x2d6a4f', '0xffd166', '0xef476f'], ['0x264653', '0x2a9d8f', '0xe9c46a'], ['0x3a0ca3', '0xf72585', '0x4cc9f0'],
	['0x1d3557', '0xa8dadc', '0xe63946'], ['0x6a4c93', '0xffca3a', '0x8ac926'], ['0x003049', '0xf77f00', '0xfcbf49'],
];
const sizes = {
	m1: [1200, 900], i21: [900, 1200], i22: [900, 1200], i31: [1000, 1400], i32: [1200, 800], i33: [1200, 800],
	v1: [540, 960], v2: [960, 540], n1: [480, 480],
};

function ffmpeg(args) {
	execFileSync('ffmpeg', ['-hide_banner', '-loglevel', 'error', '-y', ...args], { stdio: 'ignore' });
}

function mediaFile(id, kind) {
	const [width, height] = sizes[id] || [800, 800];
	const seed = hash(id);
	const colors = palettes[seed % palettes.length];
	const gradient = (duration) => `gradients=s=${width}x${height}:c0=${colors[0]}:c1=${colors[1]}:c2=${colors[2]}:nb_colors=3:seed=${seed % 9973}:speed=${duration ? 0.02 : 0.00001}${duration ? ':d=' + duration : ''}`;
	const file = path.join(CACHE, `${id}.${kind}`);
	if (fs.existsSync(file) && fs.statSync(file).size > 0) {
		return file;
	}
	try {
		if (kind === 'jpg') {
			ffmpeg(['-f', 'lavfi', '-i', gradient(0), '-frames:v', '1', '-q:v', '3', file]);
		} else if (kind === 'mp4') {
			const seconds = id === 'v1' ? 6 : 5;
			ffmpeg(['-f', 'lavfi', '-i', gradient(seconds), '-f', 'lavfi', '-i', `sine=frequency=${330 + (seed % 5) * 55}:duration=${seconds}`,
				'-c:v', 'libx264', '-preset', 'veryfast', '-pix_fmt', 'yuv420p', '-r', '24', '-c:a', 'aac', '-b:a', '64k', '-shortest', '-movflags', '+faststart', file]);
		} else if (kind === 'm4a') {
			ffmpeg(['-f', 'lavfi', '-i', 'sine=frequency=392:duration=34', '-c:a', 'aac', '-b:a', '48k', file]);
		}
	} catch (error) {
		console.error('ffmpeg failed for', id, kind, String(error));
		return null;
	}
	return file;
}

function serveMedia(res, id, variant) {
	const kind = variant === 'mp4' ? 'mp4' : variant === 'audio' ? 'm4a' : 'jpg';
	const file = mediaFile(id, kind);
	if (!file) {
		return json(res, 500, { code: 'media.failed', title: 'ffmpeg is not available' });
	}
	const type = kind === 'mp4' ? 'video/mp4' : kind === 'm4a' ? 'audio/mp4' : 'image/jpeg';
	res.writeHead(200, { 'Content-Type': type, 'Content-Length': fs.statSync(file).size });
	fs.createReadStream(file).pipe(res);
}

// ---- routes ------------------------------------------------------------------------------------

http.createServer((req, res) => {
	const chunks = [];
	req.on('data', (chunk) => chunks.push(chunk));
	req.on('end', () => {
		const body = Buffer.concat(chunks);
		const url = new URL(req.url, 'http://x');
		const p = url.pathname;
		const m = req.method;
		let mm;
		if ((mm = p.match(/^\/v1\/posts\/([^/]+)\/media\/([^/]+)$/))) {
			return serveMedia(res, decodeURIComponent(mm[2]), url.searchParams.get('variant') || 'feed');
		}
		if (p === '/v1/feed/for-you' || p === '/v1/feed/following') {
			if (url.searchParams.get('cursor')) {
				return json(res, 200, { items: [], authors: {} });
			}
			return forward(req, res, body, (page) => {
				const items = page.items || [];
				const extras = feedExtras();
				// The mock's first posts, then ours, then the rest of the mock's.
				const quoted = items.find((item) => item.post && item.post.id === 'p2');
				if (quoted) extra.px_qmedia.quote = quoted.post;
				const merged = [...items.slice(0, 3), ...extras.slice(0, 6), ...items.slice(3), ...extras.slice(6)];
				return { ...page, items: merged, authors: { ...(page.authors || {}), ...users } };
			});
		}
		if (p === '/v1/notifications' && m === 'GET' && !url.searchParams.get('before')) {
			const tab = url.searchParams.get('tab') || 'all';
			const ours = [
				{ id: 'nx1', kind: 'reply', actors: ['u_sara'], actor_count: 1, refs: { reply: 'r22', parent: 'r20', root: 'p1', post: 'r20' }, tab: 'all', updated_at: now - 60_000, read: false },
				{ id: 'nx2', kind: 'follow', actors: ['u_nika', 'u_reza', 'u_ali'], actor_count: 5, tab: 'all', updated_at: now - 120_000, read: false },
				{ id: 'nx3', kind: 'mention', actors: ['u_nika'], actor_count: 1, refs: { post: 'r21', root: 'p1' }, tab: 'mentions', updated_at: now - 180_000, read: false },
			].filter((n) => tab === 'all' || n.tab === tab);
			return forward(req, res, body, (page) => ({ ...page, notifications: [...ours, ...(page.notifications || [])] }));
		}
		if (p === '/v1/feed/video') {
			if (url.searchParams.get('cursor')) {
				return json(res, 200, { items: [], authors: {} });
			}
			return json(res, 200, { items: [{ post: extra.px_video }, { post: extra.px_video2 }], authors: users });
		}
		if (p === '/v1/feed/following/new') {
			return json(res, 200, { count: NEW_POSTS });
		}
		if (p === '/v1/posts/counts' && m === 'POST') {
			let ids = [];
			try { ids = JSON.parse(body.toString('utf8')).ids || []; } catch { /* */ }
			return forward(req, res, body, (answer) => {
				const posts = { ...(answer.posts || {}) };
				for (const id of ids) {
					if (extra[id]) posts[id] = { counts: extra[id].counts, my: extra[id].my };
				}
				return { ...answer, posts };
			});
		}
		if ((mm = p.match(/^\/v1\/posts\/([^/]+)$/)) && m === 'GET') {
			const id = decodeURIComponent(mm[1]);
			if (id === 'px_qmedia' && !extra.px_qmedia.quote) {
				req.url = '/v1/posts/p2';
				return upstream(req, null, (answer, data) => {
					try { extra.px_qmedia.quote = JSON.parse(data.toString('utf8')); } catch { /* */ }
					json(res, 200, extra.px_qmedia);
				});
			}
			if (extra[id]) return json(res, 200, extra[id]);
			if (extraReplies[id]) return json(res, 200, extraReplies[id]);
		}
		if ((mm = p.match(/^\/v1\/posts\/([^/]+)\/replies$/)) && m === 'GET') {
			const id = decodeURIComponent(mm[1]);
			if (id === 'p1') {
				if (url.searchParams.get('cursor') === 'page2') {
					return json(res, 200, { replies: pageTwo });
				}
				return forward(req, res, body, (page) => ({ replies: [...(page.replies || []), ...moreReplies], next_cursor: 'page2' }));
			}
			if (extra[id]) return json(res, 200, { replies: [] });
		}
		if ((mm = p.match(/^\/v1\/posts\/([^/]+)\/(reaction|repost|bookmark|not-interested|reply-policy|vote|share-link|reactions)$/))) {
			const id = decodeURIComponent(mm[1]);
			const post = extra[id] || extraReplies[id];
			if (post) {
				let parsed = {};
				try { parsed = body.length ? JSON.parse(body.toString('utf8')) : {}; } catch { /* */ }
				if (mm[2] === 'reaction') {
					const was = post.my.reaction;
					post.my.reaction = parsed.kind === 'none' ? undefined : parsed.kind;
					if (was === 'like') post.counts.likes--;
					if (post.my.reaction === 'like') post.counts.likes++;
					return json(res, 200, { reaction: parsed.kind });
				}
				if (mm[2] === 'repost') post.my.reposted = m === 'POST';
				if (mm[2] === 'bookmark') post.my.bookmarked = m === 'POST';
				if (mm[2] === 'share-link') return json(res, 201, { slug: 'x' + id });
				if (mm[2] === 'reactions') return json(res, 200, { users: Object.values(users).map((u) => ({ ...u, follow_state: 'none' })) });
				if (mm[2] === 'vote') return json(res, 200, post);
				return json(res, 204, {});
			}
		}
		if (p === '/v1/sounds/s1') {
			return json(res, 200, sound);
		}
		if (p === '/v1/sounds/s1/posts') {
			return json(res, 200, { posts: url.searchParams.get('before') ? [] : [extra.px_video2, extra.px_video], authors: users });
		}
		if (p === '/v1/me/bookmarks' && !url.searchParams.get('cursor')) {
			return forward(req, res, body, (page) => ({
				...page,
				posts: [...(page.posts || []), ...Object.values(extra).filter((post) => post.my.bookmarked)],
				authors: { ...(page.authors || {}), ...users },
			}));
		}
		forward(req, res, body);
	});
}).listen(PORT, '127.0.0.1', () => console.log(`mock eli proxy on http://127.0.0.1:${PORT} -> ${TARGET.origin}`));
