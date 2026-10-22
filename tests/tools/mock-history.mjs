#!/usr/bin/env node
// A stateful chat backend for the history screen, in front of the web client's
// mock API (pwa/scripts/mock-api.mjs). The proxy owns everything under
// /v1/chats, /v1/sync, /v1/stickers, /v1/link-preview, /v1/uploads, /v1/threads
// and /v1/presence; the rest (sign-in, users, posts, graph) is forwarded.
//
//   node tests/tools/mock-history.mjs              # :8102, starts the web mock on :18102
//   PORT=8102 PWA_MOCK=/path/to/mock-api.mjs node tests/tools/mock-history.mjs
//   Eleza --api=http://127.0.0.1:8102 --debug-login=+989121234567:123456 --debug-route=chat/rich
//
// Chats:
//   rich    a direct chat with every message kind: entities / spoilers, quotes, link
//           previews, reactions, a forwarded post, stickers, voice, music, files,
//           locations, calls, system messages, mixed RTL / LTR text, emoji-only, pins
//   c1..c4  the web mock's chats (c3 = a bot with an inline keyboard, c4 = an
//           anonymous conversation waiting for a listener). In c3 the bot answers
//           "/menu" with a reply keyboard, "/once" with a one-time one and «بستن»
//           by removing it
//   req     an incoming message request
//   ro      a finished conversation that can be continued in direct messages
//   gone    a chat with a deleted account
//   big     400 messages (pagination, jumps)
//   spam    a stranger wrote first: the report / block bar (action_bar "stranger")
//   sd      self-destructing media as a desktop session is sent it (a text with
//           `self_destruct`): unopened, opened with its time running, expired, and
//           the screenshot notice
//
// Behaviour:
//   * sending "fail" (or any text that starts with it) is refused with 403, so the
//     bubble becomes «ارسال نشد»;
//   * every other message is read by the peer after ~1.2 s and answered after ~2.5 s
//     in `rich` and `c1`; both arrive through /v1/sync/difference (the app polls it
//     every 5 s while the realtime connection is down);
//   * edit, delete, reactions, pins, drafts, mute, theme, accept / decline, bot
//     buttons and uploads keep their state while the process lives.
import http from 'node:http';
import zlib from 'node:zlib';
import os from 'node:os';
import fs from 'node:fs';
import path from 'node:path';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const PORT = Number(process.env.PORT || 8102);
const UPSTREAM_PORT = Number(process.env.UPSTREAM_PORT || PORT + 10000);
const HERE = path.dirname(fileURLToPath(import.meta.url));
const PWA_MOCK = process.env.PWA_MOCK
	|| ['../../../pwa', '../../../../pwa', '../../../../../pwa']
		.map((dir) => path.resolve(HERE, dir, 'scripts/mock-api.mjs'))
		.find((p) => fs.existsSync(p));
const SELF = `http://127.0.0.1:${PORT}`;
const ASSETS = path.join(os.tmpdir(), 'eleza-mock-history');
fs.mkdirSync(ASSETS, { recursive: true });

if (PWA_MOCK && !process.env.NO_UPSTREAM) {
	const child = spawn(process.execPath, [PWA_MOCK], { env: { ...process.env, PORT: String(UPSTREAM_PORT) }, stdio: 'ignore' });
	process.on('exit', () => child.kill());
	process.on('SIGINT', () => process.exit(0));
	process.on('SIGTERM', () => process.exit(0));
}

// ---- pictures ---------------------------------------------------------------------------------

function png(width, height, pixel) {
	const raw = Buffer.alloc((width * 4 + 1) * height);
	let at = 0;
	for (let y = 0; y < height; y++) {
		raw[at++] = 0;
		for (let x = 0; x < width; x++) {
			const [r, g, b, a] = pixel(x, y);
			raw[at++] = r; raw[at++] = g; raw[at++] = b; raw[at++] = a;
		}
	}
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
	return Buffer.concat([
		Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
		chunk('IHDR', ihdr),
		chunk('IDAT', zlib.deflateSync(raw, { level: 3 })),
		chunk('IEND', Buffer.alloc(0)),
	]);
}

const PALETTES = [
	[[0x06, 0x41, 0x2c], [0x7f, 0xdd, 0xa5]],
	[[0xe0, 0x9a, 0x2b], [0xd6, 0x4f, 0x45]],
	[[0x5c, 0x8f, 0xd6], [0x8e, 0x7b, 0xd0]],
	[[0x3a, 0xa3, 0xa8], [0xfc, 0xef, 0xd6]],
	[[0xc9, 0x73, 0xa6], [0x37, 0x3c, 0x4a]],
];
const pictureCache = new Map();
function picture(width, height, variant) {
	const key = `${width}x${height}:${variant}`;
	if (pictureCache.has(key)) return pictureCache.get(key);
	const [from, to] = PALETTES[Math.abs(variant) % PALETTES.length];
	const cx = width * 0.5, cy = height * 0.5, radius = Math.min(width, height) / 5;
	const data = png(width, height, (x, y) => {
		const t = (x / width + y / height) / 2;
		let r = from[0] + (to[0] - from[0]) * t, g = from[1] + (to[1] - from[1]) * t, b = from[2] + (to[2] - from[2]) * t;
		const d = Math.hypot(x - cx, y - cy);
		if (d < radius) { r = r * 0.15 + 255 * 0.85; g = g * 0.15 + 255 * 0.85; b = b * 0.15 + 255 * 0.85; }
		else if (((x + y * 2 + variant * 13) % Math.max(24, Math.round(width / 8))) < 3) { r = Math.min(255, r + 26); g = Math.min(255, g + 26); b = Math.min(255, b + 26); }
		return [r | 0, g | 0, b | 0, 255];
	});
	pictureCache.set(key, data);
	return data;
}
function fitted(w, h, max) {
	const scale = Math.min(1, max / Math.max(w, h));
	return [Math.max(1, Math.round(w * scale)), Math.max(1, Math.round(h * scale))];
}

// A sticker: a round face with a transparent background.
const stickerCache = new Map();
function stickerPng(index) {
	if (stickerCache.has(index)) return stickerCache.get(index);
	const size = 256;
	const colors = [[0xff, 0xcc, 0x33], [0x7f, 0xdd, 0xa5], [0xee, 0x8a, 0x82], [0x8e, 0x7b, 0xd0], [0x5c, 0x8f, 0xd6], [0xf4, 0xc7, 0x74]];
	const [r0, g0, b0] = colors[index % colors.length];
	const mood = index % 3; // 0 smile, 1 flat, 2 open mouth
	const data = png(size, size, (x, y) => {
		const d = Math.hypot(x - 128, y - 128);
		if (d > 118) return [0, 0, 0, 0];
		if (d > 110) return [r0 * 0.7 | 0, g0 * 0.7 | 0, b0 * 0.7 | 0, 255];
		const eye = Math.min(Math.hypot(x - 88, y - 100), Math.hypot(x - 168, y - 100));
		if (eye < 13) return [0x24, 0x28, 0x24, 255];
		const m = Math.hypot(x - 128, y - 132);
		if (mood === 0 && y > 150 && m > 52 && m < 62) return [0x24, 0x28, 0x24, 255];
		if (mood === 1 && Math.abs(y - 172) < 5 && Math.abs(x - 128) < 44) return [0x24, 0x28, 0x24, 255];
		if (mood === 2 && Math.hypot(x - 128, (y - 172) * 1.3) < 24) return [0x24, 0x28, 0x24, 255];
		const shade = 1 - d / 420;
		return [Math.min(255, r0 * shade + 20) | 0, Math.min(255, g0 * shade + 20) | 0, Math.min(255, b0 * shade + 10) | 0, 255];
	});
	stickerCache.set(index, data);
	return data;
}

function ffmpegAsset(name, args) {
	const file = path.join(ASSETS, name);
	if (!fs.existsSync(file)) {
		try {
			execFileSync('ffmpeg', ['-y', '-loglevel', 'error', ...args, file], { stdio: 'ignore', timeout: 30000 });
		} catch {
			return null;
		}
	}
	return fs.existsSync(file) ? fs.readFileSync(file) : null;
}
const videoBytes = (w, h, seconds) => ffmpegAsset(`video-${w}x${h}-${seconds}.mp4`, ['-f', 'lavfi', '-i', `testsrc2=duration=${seconds}:size=${w}x${h}:rate=15`, '-pix_fmt', 'yuv420p', '-movflags', '+faststart']);
const audioBytes = (seconds, freq) => ffmpegAsset(`tone-${seconds}-${freq}.m4a`, ['-f', 'lavfi', '-i', `sine=frequency=${freq}:duration=${seconds}`, '-c:a', 'aac', '-b:a', '48k']);

// ---- state ------------------------------------------------------------------------------------

const now = Date.now();
const MIN = 60_000, HOUR = 3600_000, DAY = 86400_000;
let pts = 100;
const updates = []; // { pts, kind, chat, seq, message?, chat_info? }
const chats = {};
const messages = {};
const unread = {};
const flags = {}; // chat -> { muted, pinned, archived, section }
const pins = {}; // chat -> [{ seq, pinned_at, scope }]
const mediaInfo = {}; // media id -> { kind, mime, size, name, bytes }
const uploads = {}; // upload id -> media id

function direct(id, peer, extra = {}) {
	chats[id] = { chat: id, kind: 'direct', state: 'active', participants: [{ ref: 'u_me', user_id: 'u_me', me: true }, { ref: peer, user_id: peer, ...(extra.bot ? { bot: true } : {}) }], last_seq: 0, peer_read_seq: 0, last_read_seq: 0, can_send: true, pins: [], ...(extra.header || {}) };
	messages[id] = [];
	unread[id] = 0;
	flags[id] = { section: 'message', ...(extra.flags || {}) };
	pins[id] = [];
}
function add(chat, sender, kind, extra = {}, at = now) {
	const c = chats[chat];
	const seq = ++c.last_seq;
	const m = { chat, seq, id: `m_${chat}_${seq}`, sender, kind, created_at: at, ...extra };
	messages[chat].push(m);
	return m;
}
const wave = (seed) => Buffer.from(Array.from({ length: 64 }, (_, i) => 40 + Math.round(200 * Math.abs(Math.sin(i * 0.37 + seed) * Math.cos(i * 0.11 + seed * 2))))).toString('base64');
const utf16 = (text, part) => ({ offset: text.indexOf(part), length: part.length });

// -- the web mock's chats
direct('c1', 'u_sara', { flags: { pinned: true } });
direct('c2', 'u_ali');
direct('c3', 'u_bot', { bot: true, flags: { muted: true } });
{
	const t0 = now - 3 * DAY;
	add('c1', 'u_sara', 'text', { text: 'سلام! چطوری؟' }, t0);
	add('c1', 'u_me', 'text', { text: 'سلام سارا، خوبم مرسی. تو چطوری؟' }, t0 + MIN);
	add('c1', 'u_sara', 'text', { text: 'منم خوبم. فردا میای کافه؟ ☕️' }, t0 + 2 * MIN);
	add('c1', 'u_me', 'text', { text: 'آره حتماً. ساعت ۵ خوبه؟' }, now - 5 * HOUR);
	add('c1', 'u_sara', 'text', { text: 'عالیه 😍 پس می‌بینمت', reactions: { '❤️': 1 } }, now - 4 * HOUR);
	add('c1', 'u_sara', 'photo', { attachment: 'media1', media: { w: 1200, h: 800, blurhash: 'LKO2?U%2Tw=w]~RBVZRi};RPxuwH' } }, now - HOUR);
	add('c1', 'u_sara', 'text', { text: 'این عکس دیروز رو ببین', reply_to_seq: 6 }, now - 58 * MIN);
	chats.c1.peer_read_seq = 4;
	chats.c1.last_read_seq = 5;
	unread.c1 = 2;
	add('c2', 'u_ali', 'text', { text: 'فایل پروژه رو فرستادم، ببین اوکیه؟' }, now - DAY * 1.2);
	add('c2', 'u_ali', 'file', { attachment: 'media2', text: 'eleza-plan.pdf', media: { size: 1_240_000, mime: 'application/pdf' } }, now - DAY * 1.1);
	add('c2', 'u_me', 'text', { text: 'ممنون، نگاه می‌کنم 🙏' }, now - DAY);
	chats.c2.last_read_seq = 3;
	chats.c2.peer_read_seq = 3;
	add('c3', 'u_bot', 'text', { text: 'سلام! برای ساختن پک استیکر /new رو بفرست.' }, now - DAY * 6);
	add('c3', 'u_me', 'text', { text: '/new' }, now - DAY * 6 + MIN);
	add('c3', 'u_bot', 'text', { text: 'می‌خواهی چه کار کنی؟', reply_markup: { inline_keyboard: [
		[{ text: 'پک تازه', kind: 'callback' }, { text: 'پک‌های من', kind: 'callback' }],
		[{ text: 'راهنما در سایت', kind: 'url', url: 'https://eleza.me/help/stickers' }],
	] } }, now - DAY * 6 + 2 * MIN);
	chats.c3.last_read_seq = 3;
	chats.c3.peer_read_seq = 2;
}
// -- an anonymous conversation waiting for its listener
chats.c4 = { chat: 'c4', kind: 'conversation', state: 'active', participants: [{ ref: 'p_a', me: true }, { ref: 'p_b', masked: true, mask_name: 'ناشناس' }], last_seq: 0, can_send: true, pins: [],
	conversation: { id: 'conv1', topic: 'یه روز خوب تو تهران', category: 'heart', state: 'active', expires_at: Math.floor(now / 1000) + 5 * 3600, speaker: { ref: 'p_a', me: true, role: 'speaker' }, listener: { ref: 'p_b', masked: true, mask_name: 'ناشناس', role: 'listener' }, my_role: 'speaker' } };
messages.c4 = []; unread.c4 = 1; flags.c4 = { section: 'conversation' }; pins.c4 = [];
add('c4', 'p_b', 'system', { system: 'conversation.joined' }, now - 31 * MIN);
add('c4', 'p_b', 'text', { text: 'سلام، منم همین حس رو داشتم امروز' }, now - 30 * MIN);
// -- a finished conversation that may continue as a direct chat
chats.ro = { chat: 'ro', kind: 'conversation', state: 'readonly', participants: [{ ref: 'p_me', me: true, masked: true, mask_name: 'رهگذر' }, { ref: 'p_x', masked: true, mask_name: 'شب‌بیدار', mask_avatar: 3 }], last_seq: 0, can_send: false, pins: [],
	conversation: { id: 'conv_ro', topic: 'کسی اینجا فیلم «جدایی» رو دیده؟', category: 'cinema', state: 'readonly', expires_at: Math.floor(now / 1000) - 3600, readonly_until: Math.floor(now / 1000) + 6 * 86400, can_continue: true, speaker: { ref: 'p_x', masked: true, mask_name: 'شب‌بیدار', role: 'speaker' }, listener: { ref: 'p_me', me: true, masked: true, role: 'listener' }, my_role: 'listener' } };
messages.ro = []; unread.ro = 0; flags.ro = { section: 'conversation' }; pins.ro = [];
add('ro', 'p_x', 'text', { text: 'پایانش رو چطور فهمیدی؟' }, now - 2 * DAY);
add('ro', 'p_me', 'text', { text: 'به نظرم عمداً باز گذاشته بود تا خودمون قضاوت کنیم.' }, now - 2 * DAY + 3 * MIN);
add('ro', 'p_x', 'text', { text: 'دقیقاً! مرسی که شنیدی 🙏' }, now - 2 * DAY + 5 * MIN);
add('ro', 'p_x', 'system', { system: 'conversation.ended' }, now - DAY);
chats.ro.last_read_seq = 4;
// -- a message request
direct('req', 'u_nika', { header: { request: 'incoming', can_send: false }, flags: { section: 'request' } });
add('req', 'u_nika', 'text', { text: 'سلام! پست‌هات دربارهٔ الزا رو دیدم، می‌شه چند تا سؤال بپرسم؟' }, now - 2 * HOUR);
unread.req = 1;
// -- a deleted account
direct('gone', 'u_gone', { header: { peer_deleted: true, can_send: false } });
chats.gone.participants[1].deleted = true;
add('gone', 'u_gone', 'text', { text: 'بعداً حرف می‌زنیم.' }, now - 20 * DAY);
add('gone', 'u_me', 'text', { text: 'باشه 👋' }, now - 20 * DAY + MIN);
chats.gone.last_read_seq = 2; chats.gone.peer_read_seq = 2;
// -- a long chat
direct('big', 'u_ali');
for (let i = 0; i < 400; i++) {
	const mine = (i % 3) === 1;
	add('big', mine ? 'u_me' : 'u_ali', 'text', { text: (mine ? 'پیام من شمارهٔ ' : 'پیام شمارهٔ ') + (i + 1) + ((i % 7) === 0 ? '\nکه دو خط دارد و کمی طولانی‌تر است تا ارتفاع ردیف‌ها یکسان نباشد.' : '') }, now - (400 - i) * 11 * MIN);
}
chats.big.last_read_seq = 400; chats.big.peer_read_seq = 400;
pins.big = [{ seq: 40, pinned_at: now - DAY, scope: 'both' }];
chats.big.pins = [40];

// -- the rich chat: every kind
direct('rich', 'u_reza');
{
	const R = 'u_reza', M = 'u_me';
	const d3 = now - 3 * DAY, d1 = now - DAY - 2 * HOUR, t = now - 3 * HOUR;
	const long = 'سلام هادی! لینک مستندات اینجاست: https://eleza.me/docs/desktop و اگر سؤالی بود به @sara بگو. ضمناً build شمارهٔ v2.4.1 روی Windows و Linux تست شد ✅ #الزا #desktop';
	add('rich', R, 'text', { text: long }, d3);
	const spoil = 'پایان فیلم را نگو! قاتل همان باغبان بود 🤫 ولی بقیه‌اش را بگو.';
	add('rich', M, 'text', { text: spoil, entities: [{ type: 'spoiler', ...utf16(spoil, 'قاتل همان باغبان بود 🤫') }], edited: true, reactions: { '😂': 1 } }, d3 + 2 * MIN);
	add('rich', R, 'text', { text: '😍🔥' }, d3 + 3 * MIN);
	add('rich', R, 'photo', { attachment: 'mr4', text: 'غروب دیروز از بام تهران 🌇', media: { w: 1200, h: 800, blurhash: 'LKO2?U%2Tw=w]~RBVZRi};RPxuwH', size: 384000, mime: 'image/jpeg' }, reactions: { '❤️': 2, '🔥': 1 }, my_reaction: '❤️' }, d3 + 10 * MIN);
	add('rich', M, 'photo', { attachment: 'mr5', media_spoiler: true, media: { w: 800, h: 1100, blurhash: 'LEHV6nWB2yk8pyo0adR*.7kCMdnj', size: 210000, mime: 'image/jpeg' } }, d3 + 12 * MIN);
	add('rich', R, 'system', { system: 'theme:ocean' }, d3 + 13 * MIN);
	add('rich', R, 'video', { attachment: 'mr7', text: 'دموی نسخهٔ دسکتاپ', media: { w: 1280, h: 720, duration_ms: 42000, blurhash: 'LGF5]+Yk^6#M@-5c,1J5@[or[Q6.', size: 5_300_000, mime: 'video/mp4' } }, d1);
	add('rich', M, 'gif', { attachment: 'mr8', media: { w: 480, h: 360, duration_ms: 3000, blurhash: 'L6PZfSi_.AyE_3t7t7R**0o#DgR4', size: 420000, mime: 'video/mp4' } }, d1 + MIN);
	add('rich', R, 'video_note', { attachment: 'mr9', media: { w: 384, h: 384, duration_ms: 17000, blurhash: 'LKO2?U%2Tw=w]~RBVZRi};RPxuwH', size: 900000, mime: 'video/mp4' } }, d1 + 2 * MIN);
	add('rich', R, 'voice', { attachment: 'mr10', media: { duration_ms: 23000, waveform: wave(1), size: 46000, mime: 'audio/ogg' } }, d1 + 5 * MIN);
	add('rich', M, 'voice', { attachment: 'mr11', played: true, media: { duration_ms: 8000, waveform: wave(2), size: 16000, mime: 'audio/ogg' } }, d1 + 6 * MIN);
	add('rich', R, 'audio', { attachment: 'mr12', text: 'shajarian-morghe-sahar.mp3', audio: { title: 'مرغ سحر', performer: 'محمدرضا شجریان' }, media: { duration_ms: 312000, size: 7_400_000, mime: 'audio/mpeg' } }, d1 + 8 * MIN);
	add('rich', R, 'file', { attachment: 'mr13', text: 'eleza-desktop-architecture.pdf', media: { size: 2_480_000, mime: 'application/pdf' } }, d1 + 20 * MIN);
	add('rich', M, 'file', { attachment: 'mr14', text: 'screenshots-windows-linux.zip', media: { size: 18_700_000, mime: 'application/zip' } }, d1 + 21 * MIN);
	add('rich', R, 'sticker', { sticker: { id: 's1_2', pack: 'pk1', emoji: '😄', url: `${SELF}/_sticker/2.png`, width: 256, height: 256 } }, d1 + 30 * MIN);
	add('rich', M, 'sticker', { reply_to_seq: 13, sticker: { id: 's1_4', pack: 'pk1', emoji: '👍', url: `${SELF}/_sticker/4.png`, width: 256, height: 256 } }, d1 + 31 * MIN);
	add('rich', R, 'location', { location: { lat: 35.7448, lng: 51.3753, label: 'برج میلاد', address: 'تهران، بزرگراه همت' } }, d1 + 40 * MIN);
	add('rich', M, 'live_location', { location: { lat: 35.7006, lng: 51.3378 }, live: { period: 3600, until: Math.floor(now / 1000) + 2400 } }, t - 30 * MIN);
	add('rich', R, 'forwarded_post', { forwarded_post: 'p2', text: 'این پست رو دیدی؟' }, t - 25 * MIN);
	add('rich', M, 'text', { text: 'آره مستندات رو خوندم، عالی بود 👌', reply_to_seq: 1, quote: { text: 'build شمارهٔ v2.4.1 روی Windows و Linux تست شد', offset: long.indexOf('build شمارهٔ') } }, t - 20 * MIN);
	const withLink = 'خبر انتشار هم اینجاست https://eleza.me/blog/desktop-beta';
	add('rich', R, 'text', { text: withLink, link_preview: { url: 'https://eleza.me/blog/desktop-beta', site_name: 'Eleza', title: 'نسخهٔ آزمایشی الزا برای دسکتاپ', description: 'الزا حالا روی ویندوز و لینوکس: پیام‌رسان، گفت‌وگوی ناشناس و الی در یک پنجره.', image_url: `${SELF}/_img/preview.png`, image_width: 1200, image_height: 630 } }, t - 15 * MIN);
	add('rich', R, 'text', { text: 'جلسهٔ فردا ساعت ۱۰ صبح برگزار می‌شود. لطفاً همه سر وقت باشند.', forwarded: true }, t - 12 * MIN);
	// Call records as the backend writes them: a system message sent by the caller.
	const call = (sender, outcome, duration, at) => add('rich', sender, 'system', { system: `call:${outcome}:${duration}`, call: { outcome, duration_s: duration } }, at);
	call(M, 'completed', 312, t - 11 * MIN);
	call(R, 'completed', 47, t - 10 * MIN - 40 * 1000);
	call(M, 'declined', 0, t - 10 * MIN - 20 * 1000);
	call(M, 'missed', 0, t - 10 * MIN);
	call(R, 'declined', 0, t - 9 * MIN - 30 * 1000);
	call(R, 'missed', 0, t - 9 * MIN);
	add('rich', R, 'text', { text: 'gif از ربات', via_bot: { id: 'u_bot', handle: 'stickerbot', name: 'ربات استیکر' } }, t - 8 * MIN);
	add('rich', M, 'system', { system: 'message.pinned:4' }, t - 7 * MIN);
	add('rich', R, 'hologram_v9', { text: 'این نوع پیام در این نسخه پشتیبانی نمی‌شود.' }, t - 6 * MIN);
	add('rich', M, 'text', { text: 'OK, see you tomorrow at 10 — I will bring the laptop.\nخط دوم فارسی است و باید راست‌چین شود.' }, t - 5 * MIN);
	add('rich', R, 'text', { text: 'عالی! پس تا فردا 🙌' }, t - 3 * MIN);
	add('rich', R, 'text', { text: 'راستی نسخهٔ جدید رو نصب کردی؟' }, t - 2 * MIN);
	add('rich', R, 'text', { text: 'اگر مشکلی دیدی همین‌جا بگو.' }, t - MIN);
	chats.rich.peer_read_seq = 27;
	chats.rich.last_read_seq = 27;
	chats.rich.theme = '';
	unread.rich = 3;
	pins.rich = [{ seq: 4, pinned_at: t - 7 * MIN, scope: 'both' }, { seq: 13, pinned_at: d1 + 22 * MIN, scope: 'both' }, { seq: 1, pinned_at: d3 + MIN, scope: 'me' }];
	chats.rich.pins = [4, 13, 1];
	chats.rich.draft = undefined;
}

const stickerPacks = [
	{ id: 'pk1', slug: 'smiles', title: 'لبخندها', cover_url: `${SELF}/_sticker/0.png`, default: true, installed: true,
		stickers: ['😀', '🙂', '😄', '😐', '👍', '😮', '😍', '😴', '🤔', '😎', '🥳', '😢'].map((emoji, i) => ({ id: `s1_${i}`, emoji, emojis: [emoji], url: `${SELF}/_sticker/${i}.png`, width: 256, height: 256 })) },
	{ id: 'pk2', slug: 'moods', title: 'حال‌وهوا', cover_url: `${SELF}/_sticker/13.png`, default: false, installed: true,
		stickers: ['🔥', '❤️', '🙏', '🎉', '☕️', '🌙'].map((emoji, i) => ({ id: `s2_${i}`, emoji, emojis: [emoji], url: `${SELF}/_sticker/${i + 12}.png`, width: 256, height: 256 })) },
	{ id: 'pk3', slug: 'eleza-girl', title: 'دختر الزا', cover_url: `${SELF}/_sticker/20.png`, default: false, installed: false,
		stickers: ['👋', '💚', '😉', '🫶'].map((emoji, i) => ({ id: `s3_${i}`, emoji, emojis: [emoji], url: `${SELF}/_sticker/${i + 20}.png`, width: 256, height: 256 })) },
];
let stickerVersion = 1;
const findSticker = (id) => {
	for (const pack of stickerPacks) {
		const found = pack.stickers.find((s) => s.id === id);
		if (found) return { id: found.id, pack: pack.id, emoji: found.emoji, url: found.url, width: found.width, height: found.height };
	}
	return null;
};

// ---- helpers ----------------------------------------------------------------------------------

const json = (res, status, body) => {
	const text = (status === 204) ? '' : JSON.stringify(body);
	res.writeHead(status, { 'Content-Type': status >= 400 ? 'application/problem+json; charset=utf-8' : 'application/json; charset=utf-8' });
	res.end(text);
};
const bytes = (res, type, data) => {
	if (!data) return json(res, 404, { code: 'media.not_found', title: 'پیدا نشد' });
	res.writeHead(200, { 'Content-Type': type, 'Content-Length': data.length });
	res.end(data);
};
// -- a stranger wrote first: the report / block bar
direct('spam', 'u_john', { header: { action_bar: 'stranger' } });
add('spam', 'u_john', 'text', { text: 'Hi! Want to earn $500 a day from home? 💸' }, now - 40 * MIN);
add('spam', 'u_john', 'text', { text: 'https://example.com/offer' }, now - 39 * MIN);
unread.spam = 2;
// -- self-destructing media
direct('sd', 'u_nika');
{
	const t0 = now - 2 * HOUR;
	add('sd', 'u_nika', 'text', { text: 'یه عکس برات فرستادم، فقط یه بار می‌تونی ببینیش' }, t0);
	add('sd', 'u_nika', 'text', { text: 'این پیام فقط روی گوشی قابل دیدن است', self_destruct: { mode: 'view_once', state: 'unopened' } }, t0 + MIN);
	add('sd', 'u_me', 'text', { text: 'این پیام فقط روی گوشی قابل دیدن است', self_destruct: { mode: 'timer', seconds: 30, state: 'unopened' } }, t0 + 5 * MIN);
	add('sd', 'u_me', 'text', { text: 'این پیام فقط روی گوشی قابل دیدن است', self_destruct: { mode: 'timer', seconds: 60, state: 'opened', opened_at: now - 20_000, destruct_at: now + 15 * MIN } }, t0 + 10 * MIN);
	add('sd', 'u_nika', 'system', { system: 'self_destruct_screenshot:u_nika:4', text: 'نیکا موسوی از پیام یکبارمصرف اسکرین‌شات گرفت' }, t0 + 11 * MIN);
	add('sd', 'u_me', 'text', { text: 'این پیام منقضی شد', self_destruct: { mode: 'view_once', state: 'expired', opened_at: t0 + 12 * MIN, destruct_at: t0 + 13 * MIN } }, t0 + 12 * MIN);
	add('sd', 'u_nika', 'text', { text: 'دیدمش 😄', reply_to_seq: 6 }, t0 + 14 * MIN);
	chats.sd.last_read_seq = 7; chats.sd.peer_read_seq = 7;
}
const header = (id) => chats[id];
const visible = (m) => !m.deleted;
function push(kind, chat, extra = {}) {
	pts++;
	updates.push({ pts, kind, chat, chat_info: header(chat), ...extra });
}
function inboxEntry(id) {
	const c = chats[id];
	const last = messages[id].filter(visible).at(-1);
	const f = flags[id];
	return { chat: id, section: f.section, state: c.state, last_seq: c.last_seq, unread: unread[id] || 0, muted: !!f.muted, pinned: !!f.pinned, archived: !!f.archived, last_activity_at: last?.created_at ?? now, last_message: last, chat_info: c, peer_deleted: !!c.peer_deleted, draft: c.draft };
}

function forward(req, res, body) {
	const upstream = http.request({ host: '127.0.0.1', port: UPSTREAM_PORT, method: req.method, path: req.url, headers: { ...req.headers, host: `127.0.0.1:${UPSTREAM_PORT}` } }, (answer) => {
		const chunks = [];
		answer.on('data', (chunk) => chunks.push(chunk));
		answer.on('end', () => {
			const headers = { ...answer.headers };
			delete headers['content-length'];
			res.writeHead(answer.statusCode, headers);
			res.end(Buffer.concat(chunks));
		});
	});
	upstream.on('error', () => json(res, 502, { code: 'proxy.unreachable', title: 'mock api is not running' }));
	upstream.end(body);
}
function forwardJson(req, res, body, rewrite) {
	const upstream = http.request({ host: '127.0.0.1', port: UPSTREAM_PORT, method: req.method, path: req.url, headers: { ...req.headers, host: `127.0.0.1:${UPSTREAM_PORT}` } }, (answer) => {
		const chunks = [];
		answer.on('data', (chunk) => chunks.push(chunk));
		answer.on('end', () => {
			try {
				json(res, answer.statusCode, rewrite(JSON.parse(Buffer.concat(chunks).toString('utf8'))));
			} catch {
				json(res, 502, { code: 'proxy.bad_answer', title: 'bad upstream answer' });
			}
		});
	});
	upstream.on('error', () => json(res, 502, { code: 'proxy.unreachable', title: 'mock api is not running' }));
	upstream.end(body);
}

function attachment(res, chat, seq, variant) {
	const m = (messages[chat] || []).find((x) => x.seq === seq);
	if (!m) return json(res, 404, { code: 'media.not_found', title: 'پیدا نشد' });
	const uploaded = mediaInfo[m.attachment];
	const w = m.media?.w || 800, h = m.media?.h || 600;
	const image = (max) => {
		if (uploaded?.bytes && /^image\//.test(uploaded.mime || '')) return bytes(res, uploaded.mime, uploaded.bytes);
		const [fw, fh] = fitted(w, h, max);
		return bytes(res, 'image/png', picture(fw, fh, seq + chat.length));
	};
	switch (variant) {
	case 'thumb': return image(320);
	case 'feed': return image(800);
	case 'full': return image(1280);
	case 'poster': return image(800);
	case 'cover': return bytes(res, 'image/png', picture(240, 240, seq));
	case 'mp4': {
		if (uploaded?.bytes) return bytes(res, uploaded.mime || 'video/mp4', uploaded.bytes);
		const [fw, fh] = fitted(w, h, 480);
		return bytes(res, 'video/mp4', videoBytes(fw - (fw % 2), fh - (fh % 2), m.kind === 'gif' ? 2 : 4));
	}
	case 'audio': {
		if (uploaded?.bytes) return bytes(res, uploaded.mime || 'audio/mp4', uploaded.bytes);
		return bytes(res, 'audio/mp4', audioBytes(Math.min(8, Math.max(2, Math.round((m.media?.duration_ms || 4000) / 1000))), 330 + (seq % 5) * 55));
	}
	case 'file':
	default: {
		if (uploaded?.bytes) return bytes(res, uploaded.mime || 'application/octet-stream', uploaded.bytes);
		if (m.kind === 'photo') return image(1280);
		return bytes(res, m.media?.mime || 'application/octet-stream', Buffer.from(`%PDF-1.4\n% ${m.text || 'file'} (mock)\n`, 'utf8'));
	}
	}
}

function sendMessage(chatId, b) {
	const c = chats[chatId];
	const existing = messages[chatId].find((x) => x.client_message_id && x.client_message_id === b.client_message_id);
	if (existing) return existing;
	const extra = { client_message_id: b.client_message_id };
	for (const key of ['text', 'reply_to_seq', 'attachment', 'entities', 'quote', 'media_spoiler', 'location', 'audio', 'forwarded_post']) {
		if (b[key] !== undefined && b[key] !== null) extra[key] = b[key];
	}
	if (b.forward_from) extra.forwarded = true;
	if (b.link_preview) extra.link_preview = linkPreview(b.link_preview);
	if (b.kind === 'sticker') extra.sticker = findSticker(b.sticker) || { id: b.sticker, pack: 'pk1', url: `${SELF}/_sticker/1.png`, width: 256, height: 256 };
	if (b.kind === 'live_location') extra.live = { period: b.live_period || 900, until: Math.floor(Date.now() / 1000) + (b.live_period || 900) };
	if (b.attachment) {
		const info = mediaInfo[b.attachment] || {};
		const source = b.forward_from ? (messages[b.forward_from.chat] || []).find((x) => x.seq === b.forward_from.seq) : null;
		extra.media = source?.media ? { ...source.media } : { size: info.size, mime: info.mime, ...(info.w ? { w: info.w, h: info.h } : {}), ...(b.duration_ms ? { duration_ms: b.duration_ms } : {}) };
		if (b.kind === 'voice' && !extra.media.waveform) extra.media.waveform = wave(c.last_seq + 3);
		if ((b.kind === 'voice' || b.kind === 'video' || b.kind === 'video_note' || b.kind === 'audio') && !extra.media.duration_ms) extra.media.duration_ms = b.media?.duration_ms || 5000;
	}
	const mySender = c.participants.find((p) => p.me)?.ref || 'u_me';
	const m = add(chatId, mySender, b.kind || 'text', extra, Date.now());
	delete c.draft; // like the server: what was sent is no draft any more
	c.last_read_seq = m.seq;
	unread[chatId] = 0;
	push('message', chatId, { seq: m.seq, message: m });
	const peer = c.participants.find((p) => !p.me)?.ref;
	if (peer && c.can_send !== false) {
		setTimeout(() => {
			c.peer_read_seq = Math.max(c.peer_read_seq || 0, m.seq);
			push('read', chatId, { seq: m.seq });
		}, 1200);
		if (chatId === 'c3' && (b.kind || 'text') === 'text') {
			// The bot's reply keyboard: "/menu" shows it, «بستن» removes it, a
			// one-time one comes with "/once".
			const text = (b.text || '').trim();
			const rows = [[{ text: 'پک تازه' }, { text: 'پک‌های من' }], [{ text: 'راهنما' }, { text: 'بستن' }]];
			const answer = (text === '/menu')
				? { text: 'یکی را انتخاب کن:', reply_markup: { keyboard: rows, resize: true, placeholder: 'یک گزینه را انتخاب کن…' } }
				: (text === '/once')
				? { text: 'بله یا نه؟', reply_markup: { keyboard: [[{ text: 'بله' }, { text: 'نه' }]], resize: true, one_time: true } }
				: (text === 'بستن')
				? { text: 'صفحه‌کلید بسته شد.', reply_markup: { remove_keyboard: true } }
				: { text: `«${text}» انتخاب شد` };
			setTimeout(() => {
				const reply = add(chatId, peer, 'text', answer, Date.now());
				push('message', chatId, { seq: reply.seq, message: reply });
			}, 600);
		}
		if (chatId === 'rich' || chatId === 'c1') {
			setTimeout(() => {
				const reply = add(chatId, peer, 'text', { text: 'باشه 👍' }, Date.now());
				unread[chatId] = (unread[chatId] || 0) + 1;
				push('message', chatId, { seq: reply.seq, message: reply });
			}, 2500);
		}
	}
	return m;
}

function linkPreview(url) {
	let normalized = String(url || '').trim();
	if (!/^https?:\/\//i.test(normalized)) normalized = 'https://' + normalized;
	let host = 'example.com';
	try { host = new URL(normalized).host; } catch { /* keep */ }
	const own = /(^|\.)eleza\.me$/.test(host);
	return { url: normalized, site_name: own ? 'Eleza' : host, title: own ? 'الزا — جایی برای گفتن؛ جایی برای شنیدن' : `صفحه‌ای در ${host}`, description: own ? 'پیام‌رسان، گفت‌وگوی ناشناس و الی، همه در یک برنامه.' : 'پیش‌نمایش پیوند از سرور آزمایشی.', image_url: `${SELF}/_img/preview.png`, image_width: 1200, image_height: 630 };
}

// ---- routes -----------------------------------------------------------------------------------

function handle(req, res, raw) {
	const url = new URL(req.url, 'http://x');
	const p = url.pathname;
	const m = req.method;
	let b = {};
	if (raw.length && (req.headers['content-type'] || '').includes('json')) {
		try { b = JSON.parse(raw.toString('utf8')); } catch { /* */ }
	}
	let mm;
	if (p === '/v1/client-config') {
		return forwardJson(req, res, raw, (config) => ({
			...config,
			'realtime.fallback_sync_interval': 5,
			'realtime.fallback_sync_after': 1,
			'feature.link_preview': true,
			'feature.stickers': true,
			'feature.drafts': true,
			'messaging.reaction_emoji_set': ['❤️', '👍', '🔥', '😂', '😮', '😢', '🙏', '👏', '🎉', '🤔', '👎', '💯'],
			'chat.mute.durations': ['1h', '8h', '2d', 'forever'],
			'chat.live_location.periods': [900, 3600, 28800],
		}));
	}
	if ((mm = p.match(/^\/_sticker\/(\d+)\.png$/))) return bytes(res, 'image/png', stickerPng(Number(mm[1])));
	if (p === '/_img/preview.png') return bytes(res, 'image/png', picture(600, 315, 2));
	if ((mm = p.match(/^\/_put\/([^/]+)$/))) {
		const media = uploads[mm[1]];
		if (media && mediaInfo[media]) mediaInfo[media].bytes = Buffer.concat([mediaInfo[media].bytes || Buffer.alloc(0), raw]);
		res.writeHead(200, { ETag: '"mock"' });
		return res.end();
	}
	if (p === '/v1/sync/state') return json(res, 200, { pts });
	if (p === '/v1/sync/difference') {
		const from = Number(url.searchParams.get('pts') || 0);
		if (process.env.MOCK_TRACE) console.log('difference', from, '->', pts);
		return json(res, 200, { pts, updates: updates.filter((u) => u.pts > from).slice(0, 100) });
	}
	if (p === '/v1/presence') {
		const list = (b.users || []).map((user) => user === 'u_reza' ? { user, status: 'online' } : user === 'u_sara' ? { user, status: 'offline', last_seen_at: Math.floor(Date.now() / 1000) - 1500 } : { user, status: 'recently' });
		return json(res, 200, { presence: list });
	}
	if (p === '/v1/users' && m === 'GET' && (url.searchParams.get('ids') || '').includes('u_gone')) {
		return forwardJson(req, res, raw, (answer) => ({ ...answer, users: [...(answer.users || []), { id: 'u_gone', deleted: true, display_name: '' }] }));
	}
	if (p === '/v1/link-preview' && m === 'POST') {
		if (/nopreview/.test(b.url || '')) return json(res, 404, { code: 'link.no_preview', title: 'پیش‌نمایشی نیست' });
		if (!/\./.test(b.url || '')) return json(res, 422, { code: 'link.invalid_url', title: 'پیوند نامعتبر است' });
		return setTimeout(() => json(res, 200, linkPreview(b.url)), 150);
	}
	if (p === '/v1/stickers/packs') {
		if (url.searchParams.get('version') === String(stickerVersion)) return json(res, 200, { version: String(stickerVersion), unchanged: true });
		return json(res, 200, { version: String(stickerVersion), packs: stickerPacks });
	}
	if ((mm = p.match(/^\/v1\/stickers\/packs\/by-slug\/([^/]+)$/))) {
		const pack = stickerPacks.find((x) => x.slug === decodeURIComponent(mm[1]));
		return pack ? json(res, 200, pack) : json(res, 404, { code: 'sticker.pack_not_found', title: 'پیدا نشد' });
	}
	if ((mm = p.match(/^\/v1\/stickers\/packs\/([^/]+)\/installed$/))) {
		const pack = stickerPacks.find((x) => x.id === mm[1]);
		if (pack) { pack.installed = (m === 'PUT'); stickerVersion++; }
		return json(res, 204, {});
	}
	if (p === '/v1/uploads' && m === 'POST') {
		const upload = 'up_' + Math.random().toString(36).slice(2, 10);
		const media = 'media_' + Math.random().toString(36).slice(2, 10);
		uploads[upload] = media;
		mediaInfo[media] = { kind: b.kind, mime: b.mime, size: b.size, name: b.filename, bytes: null, w: b.kind === 'image' ? 1000 : undefined, h: b.kind === 'image' ? 750 : undefined };
		const partSize = 5 << 20;
		const count = Math.max(1, Math.ceil((b.size || 1) / partSize));
		return json(res, 200, { upload_id: upload, media_id: media, part_size: partSize, parts: Array.from({ length: count }, (_, i) => ({ number: i + 1, url: `${SELF}/_put/${upload}` })) });
	}
	if (p.startsWith('/v1/uploads/')) return (m === 'DELETE') ? json(res, 204, {}) : setTimeout(() => json(res, 200, { state: 'ready' }), 300);
	if (p.startsWith('/v1/media/')) return json(res, 200, { state: 'ready' });
	if (p === '/v1/threads' && m === 'POST') {
		const existing = Object.values(chats).find((c) => c.kind === 'direct' && c.participants.some((x) => x.user_id === b.user_id));
		if (existing) return json(res, 200, existing);
		const id = 'd_' + String(b.user_id || 'x').replace(/^u_/, '');
		direct(id, b.user_id);
		return json(res, 200, chats[id]);
	}
	if (p === '/v1/me/pinned-chats') return json(res, 204, {});
	if (p === '/v1/chats' && m === 'GET') {
		const section = url.searchParams.get('section') || 'message';
		const list = Object.keys(chats).filter((id) => flags[id].section === section && messages[id].some(visible)).map(inboxEntry);
		return json(res, 200, { chats: url.searchParams.get('cursor') ? [] : list, next_cursor: '' });
	}
	if (!(mm = p.match(/^\/v1\/chats\/([^/]+)(\/.*)?$/))) return forward(req, res, raw);
	const id = decodeURIComponent(mm[1]);
	const rest = mm[2] || '';
	const c = chats[id];
	if (!c) return json(res, 404, { code: 'chat.not_found', title: 'گفتگو پیدا نشد' });
	const list = messages[id];
	if (rest === '' && m === 'GET') return json(res, 200, c);
	if (rest === '/messages' && m === 'GET') {
		const before = Number(url.searchParams.get('before') || 0);
		const after = Number(url.searchParams.get('after') || 0);
		const around = Number(url.searchParams.get('around') || 0);
		const limit = Math.max(1, Math.min(100, Number(url.searchParams.get('limit') || 50)));
		const all = list.filter(visible);
		let page;
		if (around) {
			const index = Math.max(0, all.findIndex((x) => x.seq >= around));
			const start = Math.max(0, index - Math.floor(limit / 2));
			page = all.slice(start, start + limit);
		} else if (after) {
			page = all.filter((x) => x.seq > after).slice(0, limit);
		} else {
			page = (before ? all.filter((x) => x.seq < before) : all).slice(-limit);
		}
		const first = page[0]?.seq ?? 0, last = page.at(-1)?.seq ?? 0;
		const body = { chat: c, messages: after ? page : page.slice().reverse(), has_more_before: all.some((x) => x.seq < first), has_more_after: all.some((x) => x.seq > last) };
		// A little latency so loading states are visible.
		return setTimeout(() => json(res, 200, body), before ? 250 : 60);
	}
	if (rest === '/messages' && m === 'POST') {
		if (/^fail/.test(b.text || '')) return json(res, 403, { code: 'chat.cannot_send', title: 'ارسال این پیام ممکن نیست' });
		if (c.can_send === false) return json(res, 403, { code: 'chat.cannot_send', title: 'نمی‌توانی به این گفتگو پیام بفرستی' });
		const msg = sendMessage(id, b);
		return setTimeout(() => json(res, 200, { message_id: msg.id, seq: msg.seq, created_at: msg.created_at, ...(msg.live ? { live_until: msg.live.until } : {}) }), 350);
	}
	if (rest === '/read') {
		c.last_read_seq = Math.max(c.last_read_seq || 0, Number(b.seq || 0));
		unread[id] = 0;
		return json(res, 204, {});
	}
	if (rest === '/draft') {
		if (process.env.MOCK_TRACE) console.log('draft', m, id, JSON.stringify(b));
		if (m === 'DELETE' || !b.text) delete c.draft;
		else c.draft = { text: b.text, reply_to_seq: b.reply_to_seq, entities: b.entities, updated_at: Date.now() };
		return json(res, 204, {});
	}
	if (rest === '/member') {
		if (b.muted !== undefined) flags[id].muted = !!b.muted;
		if (b.mute_for !== undefined) flags[id].muted = (b.mute_for !== 'off');
		if (b.archived !== undefined) flags[id].archived = !!b.archived;
		if (b.clear_history) { for (const x of list) x.deleted = true; }
		return json(res, 204, {});
	}
	if (rest === '/pin') { flags[id].pinned = (m === 'POST'); return json(res, 204, {}); }
	if (rest === '/action-bar/dismiss' && m === 'POST') { delete c.action_bar; return json(res, 204, {}); }
	if (rest === '/theme') {
		c.theme = b.theme || '';
		const who = c.participants.find((x) => x.me)?.ref || 'u_me';
		const note = add(id, who, 'system', { system: 'theme:' + (b.theme || '') }, Date.now());
		push('message', id, { seq: note.seq, message: note });
		return json(res, 204, {});
	}
	if (rest === '/accept') { delete c.request; c.can_send = true; flags[id].section = 'message'; return json(res, 200, c); }
	if (rest === '/decline') { for (const x of list) x.deleted = true; return json(res, 204, {}); }
	if (rest === '/delete-for-both') { for (const x of list) x.deleted = true; return json(res, 204, {}); }
	if (rest === '/capability') return json(res, 200, { capability: 'cap-' + id, expires_at: Math.floor(Date.now() / 1000) + 300 });
	if (rest === '/pins' && m === 'GET') {
		const out = pins[id].map((x) => ({ message: list.find((y) => y.seq === x.seq), pinned_at: x.pinned_at, scope: x.scope })).filter((x) => x.message && visible(x.message));
		return json(res, 200, { pins: out });
	}
	if (rest === '/live-locations') return json(res, 200, { shares: list.filter((x) => x.kind === 'live_location' && !x.live?.stopped).map((x) => ({ seq: x.seq, sender: x.sender, until: x.live.until, location: x.location, at: x.created_at })) });
	if ((mm = rest.match(/^\/messages\/(\d+)(\/.*)?$/))) {
		const seq = Number(mm[1]);
		const tail = mm[2] || '';
		const msg = list.find((x) => x.seq === seq);
		if (tail === '/attachment') return attachment(res, id, seq, url.searchParams.get('variant') || 'file');
		if (!msg) return json(res, 404, { code: 'message.not_found', title: 'پیام پیدا نشد' });
		if (tail === '' && m === 'GET') return json(res, 200, msg);
		if (tail === '' && m === 'PATCH') {
			msg.text = b.text;
			msg.entities = b.entities || [];
			if (b.link_preview) msg.link_preview = linkPreview(b.link_preview); else delete msg.link_preview;
			msg.edited = true;
			push('edited', id, { seq, message: msg });
			return json(res, 204, {});
		}
		if (tail === '' && m === 'DELETE') {
			msg.deleted = true;
			pins[id] = pins[id].filter((x) => x.seq !== seq);
			c.pins = pins[id].map((x) => x.seq);
			push('deleted', id, { seq, message: msg });
			return json(res, 204, {});
		}
		if (tail === '/reaction') {
			msg.reactions = msg.reactions || {};
			if (msg.my_reaction) {
				msg.reactions[msg.my_reaction] = Math.max(0, (msg.reactions[msg.my_reaction] || 1) - 1);
				if (!msg.reactions[msg.my_reaction]) delete msg.reactions[msg.my_reaction];
				delete msg.my_reaction;
			}
			if (m === 'PUT' && b.emoji) {
				msg.reactions[b.emoji] = (msg.reactions[b.emoji] || 0) + 1;
				msg.my_reaction = b.emoji;
			}
			return json(res, 204, {});
		}
		if (tail === '/pin') {
			pins[id] = pins[id].filter((x) => x.seq !== seq);
			if (m === 'POST') pins[id].unshift({ seq, pinned_at: Date.now(), scope: b.scope || url.searchParams.get('scope') || 'both' });
			c.pins = pins[id].map((x) => x.seq);
			return json(res, 204, {});
		}
		if (tail === '/played') { msg.played = true; return json(res, 204, {}); }
		if (tail === '/callback') {
			const button = msg.reply_markup?.inline_keyboard?.[b.row || 0]?.[b.col || 0];
			return setTimeout(() => json(res, 200, { answered: true, text: button ? `«${button.text}» انتخاب شد` : '', alert: false }), 400);
		}
		if (tail === '/live-location/stop') { if (msg.live) msg.live.stopped = true; push('live_stopped', id, { seq, message: msg }); return json(res, 204, {}); }
		if (tail === '/live-location') { if (b.location) msg.location = b.location; return json(res, 204, {}); }
	}
	return json(res, 404, { code: 'not_found', title: 'mock-history: ' + m + ' ' + p });
}

http.createServer((req, res) => {
	const chunks = [];
	req.on('data', (chunk) => chunks.push(chunk));
	req.on('end', () => {
		try {
			handle(req, res, Buffer.concat(chunks));
		} catch (error) {
			json(res, 500, { code: 'mock.crashed', title: String(error && error.stack || error) });
		}
	});
}).listen(PORT, '127.0.0.1', () => console.log(`mock history on ${SELF} -> web mock on :${UPSTREAM_PORT}`));
