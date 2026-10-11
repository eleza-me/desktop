#!/usr/bin/env node
// Groups and what 0.4 needs around them, in front of another mock (the chat
// backend tests/tools/mock-history.mjs, or the web client's mock API). The
// proxy owns /v1/groups, /v1/group-invites, the member search, group reports
// and people's avatars; everything else is forwarded.
//
//   node tests/tools/mock-history.mjs &                       # :8102 (chats)
//   node tests/tools/mock-groups.mjs &                        # :8103 -> :8102
//   Eleza --api=http://127.0.0.1:8103 --debug-login=+989121234567:123456 --debug-route=chat/j1team
//
//   PORT=8103 TARGET=http://127.0.0.1:8102 node tests/tools/mock-groups.mjs
//
// The client-config it answers has the shape of the real server
// ({ "version", "values": { … } }) with `feature.groups`, the limits of both
// releases of groups and `feature.video_calls`. Environment:
//
//   GROUPS=off        no `feature.groups` (the app must show no group UI)
//   PHASE=1           the first release only (no `groups.max_admins`: no admins,
//                     invite links, pins, mentions, drafts)
//   VIDEO=off         no `feature.video_calls`
//   AVATAR_DELAY=<ms> delays every avatar and group photo (the flicker test)
//   REPLY=off         nobody answers what I send
//
// Groups (I am u_me):
//   j1team   «تیم الزا»: I own it. Admins, a restricted member, pins, mentions,
//            replies, service notes, a photo, two join requests, invite links
//   j1class  «هم‌کلاسی‌ها»: an ordinary member; three unread, one mentions me; muted
//   j1quiet  «اطلاعیه‌ها»: I am restricted (the bar instead of the composer)
//   j1slow   «گپ آهسته»: slow mode, one message every 20 s (429 group.slow_mode)
//   j1big    «محلهٔ ما»: 240 members, no join / leave notes, I am an admin who
//            may ban and invite
//
// Invite links (https://eleza.me/g/<token>):
//   j1open.AAAAAAAAAAAAAAAAAAAAAA   joinable: joining adds «کتاب‌خوان‌ها» to the list
//   j1club.BBBBBBBBBBBBBBBBBBBBBB   requires approval: 202, then «pending»
//   j1team.CCCCCCCCCCCCCCCCCCCCCC   a group I am in already («Open»)
//   j1shut.DDDDDDDDDDDDDDDDDDDDDD   I am banned there
//   j1gone.EEEEEEEEEEEEEEEEEEEEEE   expired (409); anything else: 404
//
// Behaviour: every change moves the group's pts; the difference returns the
// messages whose last change is after a pts, as they are now; a text sent to
// j1team or j1class is answered after ~2 s (the answer mentions me in j1team),
// which reaches the app with the next `groups/sync` (the app has no realtime
// connection against a mock and reads it every 5 s). «fail…» is refused with
// 403 group.send_restricted. State lives as long as the process.
//
//   GET  /control/stats                 how often each avatar / photo was fetched
//   POST /control/frame {group, text}   somebody writes into a group
//   POST /control/kick  {group}         I am removed from a group
import http from 'node:http';
import zlib from 'node:zlib';

const PORT = Number(process.env.PORT || 8103);
const TARGET = new URL(process.env.TARGET || 'http://127.0.0.1:8102');
const GROUPS_ON = process.env.GROUPS !== 'off';
const PHASE2 = process.env.PHASE !== '1';
const VIDEO_ON = process.env.VIDEO !== 'off';
const AVATAR_DELAY = Number(process.env.AVATAR_DELAY || 0);
const REPLY = process.env.REPLY !== 'off';
const ME = 'u_me';
const now = Date.now();
const MIN = 60_000, HOUR = 3600_000, DAY = 86400_000;

// ---- helpers ----------------------------------------------------------------------------------

const json = (res, status, body) => {
	const text = (status === 204) ? '' : JSON.stringify(body ?? {});
	res.writeHead(status, { 'Content-Type': status >= 400 ? 'application/problem+json' : 'application/json' });
	res.end(text);
};
const problem = (res, status, code, title, extra = {}) => json(res, status, { code, title, status, ...extra });
const readBody = (req) => new Promise((resolve) => {
	const chunks = [];
	req.on('data', (chunk) => chunks.push(chunk));
	req.on('end', () => resolve(Buffer.concat(chunks)));
});
const parse = (raw) => { try { return raw.length ? JSON.parse(raw.toString('utf8')) : {}; } catch { return {}; } };

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
	upstream.on('error', () => problem(res, 502, 'proxy.unreachable', 'the mock behind this one is not running'));
	upstream.end(body);
}

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

// A portrait-like picture: two-colour background, a head and shoulders.
const faceCache = new Map();
function face(seed, size = 160) {
	const key = `${seed}:${size}`;
	if (faceCache.has(key)) return faceCache.get(key);
	let hash = 2166136261;
	for (const ch of String(seed)) { hash ^= ch.charCodeAt(0); hash = Math.imul(hash, 16777619) >>> 0; }
	const hue = hash % 360, hue2 = (hue + 40 + (hash >> 9) % 80) % 360;
	const rgb = (h, s, l) => {
		const f = (n) => { const k = (n + h / 30) % 12; return l - s * Math.min(l, 1 - l) * Math.max(-1, Math.min(k - 3, 9 - k, 1)); };
		return [f(0) * 255, f(8) * 255, f(4) * 255];
	};
	const from = rgb(hue, 0.55, 0.62), to = rgb(hue2, 0.5, 0.42), skin = rgb((hue + 180) % 360, 0.25, 0.86);
	const data = png(size, size, (x, y) => {
		const t = (x + y) / (2 * size);
		let c = [from[0] + (to[0] - from[0]) * t, from[1] + (to[1] - from[1]) * t, from[2] + (to[2] - from[2]) * t];
		const head = Math.hypot(x - size / 2, y - size * 0.4) < size * 0.2;
		const body = Math.hypot((x - size / 2) / 1.45, y - size * 1.02) < size * 0.34;
		if (head || body) c = skin;
		return [c[0] | 0, c[1] | 0, c[2] | 0, 255];
	});
	faceCache.set(key, data);
	return data;
}
// A group photo / a picture in a message: stripes over a gradient.
function picture(seed, width = 320, height = 320) {
	const key = `p:${seed}:${width}x${height}`;
	if (faceCache.has(key)) return faceCache.get(key);
	let hash = 2166136261;
	for (const ch of String(seed)) { hash ^= ch.charCodeAt(0); hash = Math.imul(hash, 16777619) >>> 0; }
	const a = [0x06 + hash % 90, 0x41 + (hash >> 7) % 90, 0x2c + (hash >> 13) % 120];
	const b = [0x7f + (hash >> 3) % 100, 0xdd - (hash >> 11) % 90, 0xa5 - (hash >> 17) % 80];
	const data = png(width, height, (x, y) => {
		const t = (x / width + y / height) / 2;
		let c = [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t];
		if (((x + y * 2) % Math.max(24, Math.round(width / 8))) < 4) c = c.map((v) => Math.min(255, v + 30));
		if (Math.hypot(x - width / 2, y - height / 2) < Math.min(width, height) / 5) c = c.map((v) => v * 0.2 + 255 * 0.8);
		return [c[0] | 0, c[1] | 0, c[2] | 0, 255];
	});
	faceCache.set(key, data);
	return data;
}

const stats = { avatars: {}, photos: {}, attachments: 0 };
const sendImage = (res, bytes) => setTimeout(() => {
	res.writeHead(200, { 'Content-Type': 'image/png', 'Cache-Control': 'private, max-age=3600' });
	res.end(bytes);
}, AVATAR_DELAY);

// ---- people -----------------------------------------------------------------------------------

// The people of the mock behind this one, and a crowd for the large group.
const people = {
	u_me: { id: 'u_me', handle: 'hadi', display_name: 'هادی', verified: true },
	u_sara: { id: 'u_sara', handle: 'sara', display_name: 'سارا احمدی', verified: true },
	u_ali: { id: 'u_ali', handle: 'ali.r', display_name: 'علی رضایی' },
	u_nika: { id: 'u_nika', handle: 'nika', display_name: 'نیکا موسوی' },
	u_reza: { id: 'u_reza', handle: 'reza.k', display_name: 'رضا کریمی', verified: true },
	u_mina: { id: 'u_mina', handle: 'mina', display_name: 'مینا رستمی', verified: true },
	u_omid: { id: 'u_omid', handle: 'omid', display_name: 'امید نوری' },
	u_john: { id: 'u_john', handle: 'john', display_name: 'John Carter' },
	u_gone: { id: 'u_gone', display_name: '', deleted: true },
};
const firstNames = ['آرش', 'بهار', 'پویا', 'ترانه', 'جواد', 'حمید', 'دریا', 'رها', 'زهرا', 'سینا', 'شیما', 'فرهاد', 'کیان', 'لیلا', 'مهسا', 'نوید'];
const lastNames = ['صادقی', 'کاظمی', 'جعفری', 'مرادی', 'حسینی', 'قاسمی', 'شریفی', 'یوسفی'];
for (let i = 1; i <= 236; i++) {
	people[`u_c${i}`] = { id: `u_c${i}`, handle: `member${i}`, display_name: `${firstNames[i % firstNames.length]} ${lastNames[(i * 7) % lastNames.length]}` };
}
function withFaces(value) {
	if (Array.isArray(value)) {
		value.forEach(withFaces);
	} else if (value && typeof value === 'object') {
		if (typeof value.id === 'string' && people[value.id] && !people[value.id].deleted
			&& 'display_name' in value && !value.avatar_id) {
			value.avatar_id = `av_${value.id}`;
		}
		if (typeof value.id === 'string' && people[value.id] && 'display_name' in value && !value.deleted) {
			// …and the same badge, whichever mock answered.
			if (people[value.id].verified) value.verified = true; else delete value.verified;
		}
		Object.values(value).forEach(withFaces);
	}
	return value;
}
const card = (id) => {
	const user = people[id];
	if (!user) return null;
	return user.deleted ? { ...user } : { ...user, avatar_id: `av_${id}` };
};

// ---- groups -----------------------------------------------------------------------------------

const ALL_RIGHTS = ['change_info', 'delete_messages', 'ban_users', 'invite_users', 'pin_messages', 'add_admins'];
const ALL_PERMS = ['send_messages', 'send_media', 'send_links', 'add_members', 'pin_messages', 'change_info'];
const DEFAULT_PERMS = ['send_messages', 'send_media', 'send_links', 'add_members'];

const groups = {};     // id -> group
const invitePool = {}; // token -> { group, requires_approval, expired }
let serial = 0;
const uuid = () => `00000000-0000-4000-8000-${String(++serial).padStart(12, '0')}`;

function makeGroup(id, title, fields = {}) {
	const group = {
		id, title, about: '', photo: '', state: 'active', created_at: now - 30 * DAY,
		pts: 0, last_seq: 0, default_permissions: [...DEFAULT_PERMS], history_visible: true,
		members: [],   // { user, role, rights, admin_title, status, denied, restricted_until, joined_at, invited_by }
		messages: [],  // MessageView + _pts (the pts of its last change) + _mine_reaction per user
		pins: [],      // { seq, pinned_by, pinned_at }, newest first
		invites: [], requests: [], member_count_extra: 0,
		// my own state
		me: { last_read_seq: 0, muted: false, muted_until: 0, notify: 'all', archived: false, pinned: false, cleared_before_seq: 0, hidden: new Set(), reactions: {}, draft: null },
		...fields,
	};
	groups[id] = group;
	return group;
}
function join(group, user, role = 'member', extra = {}) {
	group.members.push({ user, role, rights: role === 'owner' ? [...ALL_RIGHTS] : [], status: 'member', joined_at: group.created_at + group.members.length * MIN, ...(role === 'owner' ? {} : { invited_by: group.members[0]?.user }), ...extra });
}
const memberOf = (group, user) => group.members.find((m) => m.user === user && m.status === 'member');
const count = (group) => group.members.filter((m) => m.status === 'member').length + group.member_count_extra;
const small = (group) => count(group) <= 200;

// One change of the group: its pts moves by one.
function post(group, sender, kind, extra = {}, at = Date.now()) {
	const seq = ++group.last_seq;
	const message = { chat: group.id, seq, id: `m_${group.id}_${seq}`, sender, client_message_id: extra.client_message_id || uuid(), kind, created_at: at, ...extra, _pts: ++group.pts };
	group.messages.push(message);
	if (sender === ME) group.me.last_read_seq = seq;
	return message;
}
const note = (group, sender, system, at) => post(group, sender, 'system', { system }, at);
const touch = (group, message) => { message._pts = ++group.pts; };
const mention = (text, name, user) => ({ type: 'mention', offset: text.indexOf(name), length: name.length, user });

function myRights(group) {
	const me = memberOf(group, ME);
	if (!me) return [];
	return me.role === 'owner' ? [...ALL_RIGHTS] : me.role === 'admin' ? [...me.rights] : [];
}
function restrictionApplies(member) {
	return (member.denied || []).length > 0 && (!member.restricted_until || member.restricted_until * 1000 > Date.now());
}
function myPermissions(group) {
	const me = memberOf(group, ME);
	if (!me) return [];
	if (me.role === 'owner') return [...ALL_PERMS];
	const perms = new Set(group.default_permissions);
	if (me.role === 'admin') {
		['send_messages', 'send_media', 'send_links'].forEach((p) => perms.add(p));
		if (me.rights.includes('change_info')) perms.add('change_info');
		if (me.rights.includes('pin_messages')) perms.add('pin_messages');
		if (me.rights.includes('invite_users')) perms.add('add_members');
	} else if (restrictionApplies(me)) {
		me.denied.forEach((p) => perms.delete(p));
	}
	return ALL_PERMS.filter((p) => perms.has(p));
}
const visible = (group, message) => message.seq > group.me.cleared_before_seq && !group.me.hidden.has(message.seq);
function unreadOf(group) {
	return group.messages.filter((m) => m.seq > group.me.last_read_seq && m.sender !== ME && !m.deleted && visible(group, m)).length;
}
function addresses(group, message) {
	if (message.sender === ME || message.deleted) return false;
	if ((message.entities || []).some((e) => e.type === 'mention' && e.user === ME)) return true;
	const target = message.reply_to_seq && group.messages.find((m) => m.seq === message.reply_to_seq);
	return !!target && target.sender === ME;
}
const mentionsOf = (group) => PHASE2 ? group.messages.filter((m) => m.seq > group.me.last_read_seq && visible(group, m) && addresses(group, m)).map((m) => m.seq) : [];

function messageView(group, message) {
	const { _pts, ...view } = message;
	if (message.deleted) {
		return { chat: view.chat, seq: view.seq, id: view.id, sender: view.sender, client_message_id: view.client_message_id, kind: view.kind, created_at: view.created_at, deleted: true };
	}
	const mine = group.me.reactions[message.seq];
	return mine ? { ...view, my_reaction: mine } : view;
}
const frameView = (group, message) => { const { my_reaction, ...view } = messageView(group, message); return view; };

function groupView(group) {
	const me = memberOf(group, ME);
	const permissions = myPermissions(group);
	const rights = myRights(group);
	const canSend = permissions.includes('send_messages') && !(group.slow && group.slow.until > Date.now());
	const view = {
		group: group.id, title: group.title, member_count: count(group), state: 'active', status: me ? 'member' : 'left',
		role: me?.role || 'member', rights, permissions, default_permissions: group.default_permissions,
		can_send: canSend, slow_mode_s: group.slow ? group.slow.seconds : 0, history_visible: group.history_visible,
		last_seq: group.last_seq, pts: group.pts, last_read_seq: group.me.last_read_seq, unread: unreadOf(group),
		unread_mentions: mentionsOf(group).length, notify: group.me.notify,
		pins: PHASE2 ? group.pins.map((p) => p.seq) : [], created_at: group.created_at,
	};
	if (group.about) view.about = group.about;
	if (group.photo) view.photo = group.photo;
	if (!canSend) {
		view.send_blocked = permissions.includes('send_messages') ? 'slow_mode' : 'restricted';
		if (view.send_blocked === 'slow_mode') view.slow_mode_wait_s = Math.ceil((group.slow.until - Date.now()) / 1000);
	}
	if (me && restrictionApplies(me) && me.restricted_until) view.restricted_until = me.restricted_until;
	if (group.me.muted) view.muted = true;
	if (group.me.muted_until) view.muted_until = group.me.muted_until;
	if (group.me.archived) view.archived = true;
	if (group.me.pinned) view.pinned = true;
	if (group.me.cleared_before_seq) view.cleared_before_seq = group.me.cleared_before_seq;
	if (PHASE2 && rights.includes('ban_users') && group.requests.length) view.join_requests = group.requests.length;
	return view;
}
function entryOf(group) {
	const last = [...group.messages].reverse().find((m) => visible(group, m));
	const entry = { group: groupView(group), last_activity_at: group.messages.at(-1)?.created_at || group.created_at };
	if (last) entry.last_message = messageView(group, last);
	if (PHASE2 && group.me.draft) entry.draft = group.me.draft;
	return entry;
}
function memberView(member) {
	const view = { user: member.user, role: member.role, rights: member.role === 'owner' ? [...ALL_RIGHTS] : (member.rights || []), status: member.status };
	if (member.admin_title) view.admin_title = member.admin_title;
	if (restrictionApplies(member) && member.status === 'member') {
		view.denied = member.denied;
		if (member.restricted_until) view.restricted_until = member.restricted_until;
	}
	if (member.joined_at) view.joined_at = member.joined_at;
	if (member.invited_by) view.invited_by = member.invited_by;
	return view;
}
const inviteView = (group, invite) => {
	const view = { link: invite.link, url: `https://eleza.me/g/${invite.link}`, uses: invite.uses, created_by: invite.created_by, created_at: invite.created_at };
	if (invite.name) view.name = invite.name;
	if (invite.max_uses) view.max_uses = invite.max_uses;
	if (invite.expires_at) view.expires_at = invite.expires_at;
	if (invite.requires_approval) view.requires_approval = true;
	if ((invite.expires_at && invite.expires_at * 1000 < Date.now()) || (invite.max_uses && invite.uses >= invite.max_uses)) view.expired = true;
	return view;
};
const secret = (seed) => { let out = ''; let h = 7; for (let i = 0; i < 22; i++) { h = (h * 31 + seed.charCodeAt(i % seed.length) + i * 17) >>> 0; out += 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_'[h % 64]; } return out; };
function addInvite(group, fields = {}) {
	const invite = { link: `${group.id}.${fields.secret || secret(group.id + group.invites.length + Date.now())}`, uses: 0, created_by: ME, created_at: Date.now(), ...fields };
	delete invite.secret;
	group.invites.unshift(invite);
	invitePool[invite.link] = { group: group.id, invite };
	return invite;
}

// -- «تیم الزا»: I own it
{
	const g = makeGroup('j1team', 'تیم الزا', { about: 'هماهنگی کارهای الزا 🌱\nراهنما: https://eleza.me/help', photo: 'f1team', created_at: now - 40 * DAY });
	join(g, 'u_me', 'owner');
	join(g, 'u_sara', 'admin', { rights: ['delete_messages', 'ban_users', 'invite_users', 'pin_messages'], admin_title: 'ناظر' });
	join(g, 'u_ali');
	join(g, 'u_reza', 'admin', { rights: ['pin_messages'] });
	join(g, 'u_nika', 'member', { denied: ['send_media', 'send_links'], restricted_until: Math.floor(Date.now() / 1000) + 5 * 3600 });
	join(g, 'u_john');
	const t0 = now - 2 * DAY;
	note(g, 'u_me', 'group_created', g.created_at);
	note(g, 'u_me', 'member_added:u_sara', g.created_at + MIN);
	note(g, 'u_me', 'member_added:u_ali', g.created_at + 2 * MIN);
	post(g, 'u_sara', 'text', { text: 'سلام به همه! جلسهٔ این هفته پنجشنبه ساعت ۱۰ است.' }, t0);
	post(g, 'u_sara', 'text', { text: 'دستور جلسه را تا فردا می‌فرستم.' }, t0 + MIN);
	post(g, 'u_ali', 'text', { text: 'عالی. من گزارش نسخهٔ دسکتاپ را آماده می‌کنم.', reply_to_seq: 4 }, t0 + 6 * MIN);
	post(g, 'u_me', 'text', { text: 'ممنون از هر دو 🙏' }, t0 + 8 * MIN);
	{
		const text = 'هادی نسخهٔ تازه را دیدی؟ به‌نظرم فهرست گفتگوها خیلی روان شده.';
		post(g, 'u_reza', 'text', { text, entities: [mention(text, 'هادی', 'u_me')], reactions: { '👍': 2, '🔥': 1 } }, t0 + 3 * HOUR);
	}
	post(g, 'u_reza', 'photo', { attachment: 'f1shot', text: 'این هم عکس صفحهٔ تازه', media: { width: 1200, height: 800, blurhash: 'LKO2?U%2Tw=w]~RBVZRi};RPxuwH', mime: 'image/png', size: 48000 } }, t0 + 3 * HOUR + MIN);
	post(g, 'u_john', 'text', { text: 'Looks great! Is the English layout checked as well?' }, t0 + 4 * HOUR);
	{
		const text = 'بله John، هر دو جهت بررسی شده.';
		post(g, 'u_me', 'text', { text, reply_to_seq: 10, entities: [mention(text, 'John', 'u_john')] }, t0 + 4 * HOUR + 2 * MIN);
	}
	note(g, 'u_john', 'member_joined', now - DAY);
	post(g, 'u_nika', 'text', { text: 'من هم رسیدم 👋' }, now - DAY + 5 * MIN);
	post(g, 'u_sara', 'text', { text: 'قوانین گروه: محترمانه بنویسیم، خارج از موضوع نرویم، پیام‌های مهم سنجاق می‌شوند.' }, now - 20 * HOUR);
	g.pins.unshift({ seq: 14, pinned_by: 'u_sara', pinned_at: now - 20 * HOUR + MIN });
	note(g, 'u_sara', 'pinned:14', now - 20 * HOUR + MIN);
	post(g, 'u_gone', 'text', { text: 'پیامی از حسابی که دیگر نیست.' }, now - 19 * HOUR);
	post(g, 'u_ali', 'text', { text: 'این پیام در دست بررسی است', under_review: true }, now - 18 * HOUR);
	delete g.messages.at(-1).text;
	post(g, 'u_ali', 'voice', { attachment: 'f1voice', media: { duration_ms: 7000, mime: 'audio/ogg', size: 21000 } }, now - 5 * HOUR);
	post(g, 'u_sara', 'text', { text: 'فایل برنامه را هم گذاشتم 👇' }, now - 2 * HOUR);
	post(g, 'u_sara', 'file', { attachment: 'f1plan', text: 'eleza-0.4-plan.pdf', media: { size: 1_240_000, mime: 'application/pdf' } }, now - 2 * HOUR + MIN);
	g.pins.unshift({ seq: 20, pinned_by: 'u_me', pinned_at: now - 2 * HOUR + 2 * MIN });
	note(g, 'u_me', 'pinned:20', now - 2 * HOUR + 2 * MIN);
	post(g, 'u_me', 'text', { text: 'دیدم، عالی بود.' }, now - HOUR);
	g.requests.push({ user: 'u_mina', created_at: now - 3 * HOUR, link_name: 'همه' }, { user: 'u_omid', created_at: now - 40 * MIN });
	addInvite(g, { name: 'همه', max_uses: 10, secret: 'CCCCCCCCCCCCCCCCCCCCCC' });
	g.invites[0].uses = 3;
	addInvite(g, { name: 'با تأیید', requires_approval: true, expires_at: Math.floor(Date.now() / 1000) + 7 * 86400 });
	g.members.push({ user: 'u_omid', role: 'member', rights: [], status: 'banned' });
}
// -- «هم‌کلاسی‌ها»: an ordinary member, unread with a mention
{
	const g = makeGroup('j1class', 'هم‌کلاسی‌ها', { about: 'ورودی‌های ۹۸', created_at: now - 90 * DAY });
	join(g, 'u_nika', 'owner');
	join(g, 'u_me');
	join(g, 'u_sara');
	join(g, 'u_ali');
	note(g, 'u_nika', 'group_created', g.created_at);
	post(g, 'u_nika', 'text', { text: 'بچه‌ها دورهمی جمعه سر جاشه؟' }, now - 3 * DAY);
	post(g, 'u_me', 'text', { text: 'من هستم.' }, now - 3 * DAY + 10 * MIN);
	post(g, 'u_ali', 'text', { text: 'منم میام 🙌' }, now - 50 * MIN);
	{
		const text = 'هادی تو کیک رو میاری؟';
		post(g, 'u_sara', 'text', { text, entities: [mention(text, 'هادی', 'u_me')] }, now - 45 * MIN);
	}
	post(g, 'u_nika', 'sticker', { sticker: { id: 'st3', pack: 'p1', emoji: '🎂', url: '/media/sticker/3.png', width: 256, height: 256 } }, now - 40 * MIN);
	g.me.last_read_seq = 3;
	g.me.muted = true;
}
// -- «اطلاعیه‌ها»: I am restricted
{
	const g = makeGroup('j1quiet', 'اطلاعیه‌ها', { created_at: now - 12 * DAY });
	join(g, 'u_reza', 'owner');
	join(g, 'u_me', 'member', { denied: [...ALL_PERMS], restricted_until: Math.floor(Date.now() / 1000) + 3 * 3600 });
	join(g, 'u_sara');
	note(g, 'u_reza', 'group_created', g.created_at);
	post(g, 'u_reza', 'text', { text: 'فقط اطلاعیه‌های مهم اینجا گذاشته می‌شود.' }, now - 6 * DAY);
	post(g, 'u_reza', 'text', { text: 'نسخهٔ ۰٫۴ دسکتاپ این هفته منتشر می‌شود 🎉' }, now - 26 * HOUR);
	g.me.last_read_seq = g.last_seq;
}
// -- «گپ آهسته»: slow mode
{
	const g = makeGroup('j1slow', 'گپ آهسته', { created_at: now - 5 * DAY, slow: { seconds: 20, until: 0 } });
	join(g, 'u_sara', 'owner');
	join(g, 'u_me');
	note(g, 'u_sara', 'group_created', g.created_at);
	post(g, 'u_sara', 'text', { text: 'اینجا هر ۲۰ ثانیه یک پیام می‌شود فرستاد.' }, now - 4 * DAY);
	g.me.last_read_seq = g.last_seq;
}
// -- «محلهٔ ما»: a large group, I am an admin
{
	const g = makeGroup('j1big', 'محلهٔ ما', { about: 'گروه همسایه‌ها', photo: 'f1big', created_at: now - 200 * DAY });
	join(g, 'u_mina', 'owner');
	join(g, 'u_me', 'admin', { rights: ['ban_users', 'invite_users', 'delete_messages'] });
	join(g, 'u_omid', 'admin', { rights: ['delete_messages'], admin_title: 'مدیر' });
	for (let i = 1; i <= 236; i++) join(g, `u_c${i}`);
	g.members.push({ user: 'u_ali', role: 'member', rights: [], status: 'banned' });
	note(g, 'u_mina', 'group_created', g.created_at);
	for (let i = 0; i < 180; i++) {
		const sender = `u_c${1 + (i * 7) % 236}`;
		post(g, sender, 'text', { text: i % 9 === 0 ? `کسی از قطعی آب خبر دارد؟ (${i + 1})` : `پیام شمارهٔ ${i + 1} در گروه محله` }, now - 6 * DAY + i * 37 * MIN);
	}
	g.me.last_read_seq = g.last_seq - 12;
	addInvite(g, { name: 'همسایه‌ها' });
}
// -- behind invite links: not in my list
{
	const open = makeGroup('j1open', 'کتاب‌خوان‌ها', { about: 'هر ماه یک کتاب 📚', photo: 'f1open', created_at: now - 9 * DAY });
	join(open, 'u_nika', 'owner');
	join(open, 'u_sara');
	join(open, 'u_reza');
	note(open, 'u_nika', 'group_created', open.created_at);
	post(open, 'u_nika', 'text', { text: 'کتاب این ماه: «سمفونی مردگان»' }, now - 2 * DAY);
	invitePool['j1open.AAAAAAAAAAAAAAAAAAAAAA'] = { group: 'j1open', invite: { link: 'j1open.AAAAAAAAAAAAAAAAAAAAAA', uses: 2 } };
	const club = makeGroup('j1club', 'باشگاه عکاسی', { about: 'عضویت با تأیید مدیر', created_at: now - 60 * DAY, member_count_extra: 57 });
	join(club, 'u_sara', 'owner');
	note(club, 'u_sara', 'group_created', club.created_at);
	invitePool['j1club.BBBBBBBBBBBBBBBBBBBBBB'] = { group: 'j1club', invite: { link: 'j1club.BBBBBBBBBBBBBBBBBBBBBB', uses: 0, requires_approval: true } };
	const shut = makeGroup('j1shut', 'گروه بسته', { created_at: now - 60 * DAY, member_count_extra: 11 });
	join(shut, 'u_reza', 'owner');
	shut.members.push({ user: ME, role: 'member', rights: [], status: 'banned' });
	invitePool['j1shut.DDDDDDDDDDDDDDDDDDDDDD'] = { group: 'j1shut', invite: { link: 'j1shut.DDDDDDDDDDDDDDDDDDDDDD', uses: 0 } };
	invitePool['j1gone.EEEEEEEEEEEEEEEEEEEEEE'] = { group: 'j1open', invite: { link: 'j1gone.EEEEEEEEEEEEEEEEEEEEEE', uses: 0 }, expired: true };
}
const pendingJoins = new Set();
const mine = () => Object.values(groups).filter((group) => memberOf(group, ME));

// Somebody answers what I wrote (the app learns of it with its next groups/sync).
function answer(group, mineMessage) {
	if (!REPLY || !['j1team', 'j1class'].includes(group.id) || mineMessage.kind !== 'text') return;
	setTimeout(() => {
		if (!memberOf(group, ME)) return;
		if (group.id === 'j1team') {
			const text = 'هادی دریافت شد ✅';
			post(group, 'u_sara', 'text', { text, reply_to_seq: mineMessage.seq, entities: [mention(text, 'هادی', ME)] });
		} else {
			post(group, 'u_ali', 'text', { text: 'باشه 👌' });
		}
	}, 2000);
}

// ---- routes -----------------------------------------------------------------------------------

function configValues(flat) {
	const values = { ...flat };
	delete values.version;
	Object.assign(values, {
		'realtime.fallback_sync_interval': '5s',
		'feature.drafts': true,
		'chat.mute.durations': flat['chat.mute.durations'] || ['1h', '8h', '2d', 'forever'],
		'messaging.reaction_emoji_set': flat['messaging.reaction_emoji_set'] || ['❤️', '👍', '🔥', '😂', '😮', '😢', '🙏', '👏'],
	});
	if (VIDEO_ON) values['feature.video_calls'] = true;
	if (GROUPS_ON) {
		Object.assign(values, {
			'feature.groups': true, 'groups.max_members': 5000, 'groups.max_per_user': 500,
			'groups.create.max_initial_members': 200, 'groups.members.add_batch_max': 100,
			'groups.title_max_chars': 128, 'groups.about_max_chars': 255, 'groups.members.page_size': 100,
			'groups.delete_own_window': '0s', 'groups.difference_max_messages': 200,
		});
		if (PHASE2) {
			Object.assign(values, {
				'groups.max_admins': 50, 'groups.admin_title_max_chars': 16, 'groups.invites.max_active_per_group': 100,
				'groups.invites.max_expiry': '8760h0m0s', 'groups.invites.name_max_chars': 32, 'groups.invites.link_base': 'eleza.me/g/',
				'groups.pins.max_per_group': 100, 'groups.mentions.max_per_message': 50, 'groups.mentions.page_size': 100,
				'groups.bulk_delete.max_messages': 100, 'groups.slow_mode.values': '0,10s,30s,1m,5m,15m,1h',
			});
		}
	}
	return values;
}

function page(list, cursor, limit) {
	const from = Math.max(0, Number(cursor) || 0);
	const slice = list.slice(from, from + limit);
	const out = { items: slice };
	if (from + limit < list.length) out.next = String(from + limit);
	return out;
}

async function handleGroups(req, res, url, raw) {
	const p = url.pathname, m = req.method, q = url.searchParams;
	const body = parse(raw);
	if (!GROUPS_ON) return problem(res, 403, 'group.disabled', 'گروه‌ها فعال نیست');
	let match;

	if (p === '/v1/groups' && m === 'GET') {
		const list = mine().map(entryOf).sort((a, b) => (Number(!!b.group.pinned) - Number(!!a.group.pinned)) || (b.last_activity_at - a.last_activity_at));
		const { items, next } = page(list, q.get('cursor'), Math.min(Number(q.get('limit')) || 30, 50));
		return json(res, 200, next ? { groups: items, next_cursor: next } : { groups: items });
	}
	if (p === '/v1/groups' && m === 'POST') {
		const title = String(body.title || '').trim();
		if (!title || title.length > 128) return problem(res, 400, 'group.title_invalid', 'نام گروه درست نیست');
		const group = makeGroup(`j1n${Object.keys(groups).length + 1}`, title, { about: String(body.about || ''), photo: body.photo || '', created_at: Date.now() });
		join(group, ME, 'owner');
		note(group, ME, 'group_created');
		const notAdded = [];
		for (const user of [...new Set(body.members || [])]) {
			if (user === ME) continue;
			if (!people[user]) return problem(res, 404, 'user.unavailable', 'کاربر در دسترس نیست');
			if (user === 'u_reza') { notAdded.push({ user, reason: 'privacy' }); continue; }
			join(group, user);
			note(group, ME, `member_added:${user}`);
		}
		return json(res, 201, { group: groupView(group), not_added: notAdded });
	}
	if (p === '/v1/groups/sync' && m === 'POST') {
		const known = new Map((body.known || []).map((item) => [item.group, Number(item.pts)]));
		const out = [], removed = [];
		for (const group of mine()) {
			if (!known.has(group.id) || known.get(group.id) !== group.pts) out.push(entryOf(group));
		}
		for (const id of known.keys()) {
			if (!groups[id] || !memberOf(groups[id], ME)) removed.push(id);
		}
		return json(res, 200, { groups: out, removed, count: mine().length });
	}

	// ---- invite links by token (I am usually not a member)
	if ((match = p.match(/^\/v1\/group-invites\/([^/]+)(\/join|\/photo)?$/))) {
		if (!PHASE2) return problem(res, 404, 'not_found', 'not found');
		const token = decodeURIComponent(match[1]);
		const found = invitePool[token];
		if (!found || found.invite.revoked) return problem(res, 404, 'group.invite_invalid', 'این پیوند دیگر معتبر نیست');
		if (found.expired) return problem(res, 409, 'group.invite_expired', 'مهلت این پیوند تمام شده');
		const group = groups[found.group];
		const banned = group.members.some((x) => x.user === ME && x.status === 'banned');
		if (match[2] === '/photo') {
			stats.photos[group.id] = (stats.photos[group.id] || 0) + 1;
			return sendImage(res, picture(group.photo || group.id));
		}
		if (match[2] === '/join' && m === 'POST') {
			if (banned) return problem(res, 403, 'group.banned', 'نمی‌توانی به این گروه بپیوندی');
			if (memberOf(group, ME)) return json(res, 200, { group: groupView(group) });
			if (found.invite.requires_approval) {
				pendingJoins.add(group.id);
				return json(res, 202, { pending: true });
			}
			join(group, ME);
			group.members.at(-1).invited_by = undefined;
			found.invite.uses++;
			note(group, ME, 'member_joined');
			group.me.last_read_seq = group.last_seq - 1;
			return json(res, 200, { group: groupView(group) });
		}
		const preview = { title: group.title, member_count: count(group), status: memberOf(group, ME) ? 'member' : banned ? 'banned' : pendingJoins.has(group.id) ? 'pending' : 'joinable' };
		if (group.about) preview.about = group.about;
		if (group.photo) preview.photo = group.photo;
		if (found.invite.requires_approval) preview.requires_approval = true;
		if (preview.status === 'member') preview.group = group.id;
		return json(res, 200, preview);
	}

	if (!(match = p.match(/^\/v1\/groups\/([^/]+)(\/.*)?$/))) return problem(res, 404, 'not_found', 'not found');
	const group = groups[decodeURIComponent(match[1])];
	const rest = match[2] || '';
	const me = group && memberOf(group, ME);
	if (!group || !me) return problem(res, 404, 'group.not_found', 'گروه پیدا نشد');
	const rights = myRights(group), permissions = myPermissions(group);
	// Both answer the request and return true when the caller may not go on.
	const need = (ok) => {
		if (ok) return false;
		problem(res, 403, 'group.right_required', 'اجازهٔ این کار را نداری');
		return true;
	};
	const target = (user) => group.members.find((x) => x.user === user);
	const actOn = (user) => {
		const t = target(user);
		if (user === ME) {
			problem(res, 400, 'request.invalid', 'درخواست نادرست');
			return true;
		}
		if (t && t.status === 'member' && (t.role === 'owner' || (t.role === 'admin' && me.role !== 'owner'))) {
			problem(res, 403, 'group.right_required', 'اجازهٔ این کار را نداری');
			return true;
		}
		return false;
	};
	const info = (data = {}) => { ++group.pts; return data; };

	if (rest === '' && m === 'GET') return json(res, 200, groupView(group));
	if (rest === '' && m === 'PATCH') {
		if (need(permissions.includes('change_info'))) return;
		if (body.title !== undefined && body.title !== group.title) { group.title = String(body.title).trim(); note(group, ME, 'title_changed'); }
		if (body.photo !== undefined && body.photo !== group.photo) { group.photo = body.photo; note(group, ME, 'photo_changed'); }
		if (body.about !== undefined && body.about !== group.about) { group.about = String(body.about); info(); }
		return json(res, 200, groupView(group));
	}
	if (rest === '' && m === 'DELETE') {
		if (need(me.role === 'owner')) return;
		delete groups[group.id];
		return json(res, 204);
	}
	if (rest === '/photo') {
		stats.photos[group.id] = (stats.photos[group.id] || 0) + 1;
		return sendImage(res, picture(group.photo || group.id));
	}
	if (rest === '/permissions' && m === 'PUT') {
		if (!PHASE2) return problem(res, 404, 'not_found', 'not found');
		if (body.default_permissions !== undefined) {
			if (need(rights.includes('ban_users'))) return;
			if (body.default_permissions.some((x) => !ALL_PERMS.includes(x))) return problem(res, 400, 'group.rights_invalid', 'نام اجازه درست نیست');
			group.default_permissions = ALL_PERMS.filter((x) => body.default_permissions.includes(x));
		}
		if (body.history_visible !== undefined) {
			if (need(rights.includes('change_info'))) return;
			group.history_visible = !!body.history_visible;
		}
		info();
		return json(res, 200, groupView(group));
	}
	if (rest === '/members' && m === 'GET') {
		const filter = q.get('filter') || 'all';
		if ((filter === 'restricted' || filter === 'banned') && need(rights.includes('ban_users'))) return;
		let list = group.members.filter((x) => x.status === 'member');
		if (filter === 'admins') list = list.filter((x) => x.role !== 'member');
		if (filter === 'restricted') list = list.filter(restrictionApplies);
		if (filter === 'banned') list = group.members.filter((x) => x.status === 'banned');
		const { items, next } = page(list.map(memberView), q.get('cursor'), Math.min(Number(q.get('limit')) || 50, 100));
		return json(res, 200, next ? { members: items, next_cursor: next } : { members: items });
	}
	if (rest === '/members' && m === 'POST') {
		if (need(permissions.includes('add_members'))) return;
		const added = [], notAdded = [];
		for (const user of [...new Set(body.users || [])]) {
			const t = target(user);
			if (!people[user]) return problem(res, 404, 'user.unavailable', 'کاربر در دسترس نیست');
			if (t && t.status === 'member') continue;
			if (t && t.status === 'banned') { notAdded.push({ user, reason: 'banned' }); continue; }
			if (t && t.status === 'left') { notAdded.push({ user, reason: 'left' }); continue; }
			if (user === 'u_reza') { notAdded.push({ user, reason: 'privacy' }); continue; }
			if (t) Object.assign(t, { status: 'member', role: 'member', rights: [], invited_by: ME }); else join(group, user, 'member', { invited_by: ME, joined_at: Date.now() });
			added.push(user);
			if (small(group)) note(group, ME, `member_added:${user}`); else info();
		}
		return json(res, 200, { added, not_added: notAdded });
	}
	if ((match = rest.match(/^\/members\/([^/]+)(\/admin|\/restrictions|\/ban)?$/))) {
		const user = decodeURIComponent(match[1]), what = match[2] || '';
		if (!people[user]) return problem(res, 404, 'user.unavailable', 'کاربر در دسترس نیست');
		const t = target(user);
		if (what === '' && m === 'DELETE') {
			if (need(rights.includes('ban_users')) || actOn(user)) return;
			if (t && t.status === 'member') { t.status = 'kicked'; if (small(group)) note(group, ME, `member_removed:${user}`); else info(); }
			return json(res, 204);
		}
		if (!PHASE2) return problem(res, 404, 'not_found', 'not found');
		if (what === '/admin') {
			if (need(rights.includes('add_admins')) || actOn(user)) return;
			if (!t || t.status !== 'member') return problem(res, 409, 'group.target_not_member', 'این کاربر عضو گروه نیست');
			if (m === 'DELETE') {
				if (t.role === 'admin') { t.role = 'member'; t.rights = []; delete t.admin_title; info(); }
				return json(res, 200, memberView(t));
			}
			const wanted = body.rights || [];
			if (wanted.some((x) => !ALL_RIGHTS.includes(x))) return problem(res, 400, 'group.rights_invalid', 'نام حق درست نیست');
			if (wanted.some((x) => !rights.includes(x))) return problem(res, 403, 'group.right_required', 'اجازهٔ این کار را نداری');
			if (body.title !== undefined && [...String(body.title)].length > 16) return problem(res, 400, 'group.admin_title_invalid', 'عنوان درست نیست');
			if (t.role !== 'admin' && group.members.filter((x) => x.status === 'member' && x.role === 'admin').length >= 50) return problem(res, 409, 'group.too_many_admins', 'مدیرهای گروه کامل است');
			const promoted = t.role !== 'admin';
			t.role = 'admin';
			t.rights = ALL_RIGHTS.filter((x) => wanted.includes(x));
			if (body.title !== undefined) { if (body.title) t.admin_title = String(body.title); else delete t.admin_title; }
			t.denied = [];
			if (promoted) info();
			return json(res, 200, memberView(t));
		}
		if (what === '/restrictions' && m === 'PUT') {
			if (need(rights.includes('ban_users'))) return;
			if (!t || t.status !== 'member') return problem(res, 409, 'group.target_not_member', 'این کاربر عضو گروه نیست');
			if (t.role !== 'member' || user === ME) return problem(res, 403, 'group.right_required', 'اجازهٔ این کار را نداری');
			const denied = body.denied || [];
			if (denied.some((x) => !ALL_PERMS.includes(x)) || (body.until && body.until * 1000 < Date.now())) return problem(res, 400, 'group.restriction_invalid', 'محدودیت درست نیست');
			t.denied = ALL_PERMS.filter((x) => denied.includes(x));
			t.restricted_until = denied.length ? (body.until || 0) : 0;
			return json(res, 200, memberView(t));
		}
		if (what === '/ban' && m === 'POST') {
			if (need(rights.includes('ban_users')) || actOn(user)) return;
			if (body.delete_messages && need(rights.includes('delete_messages'))) return;
			const was = t && t.status === 'member';
			if (t) t.status = 'banned'; else group.members.push({ user, role: 'member', rights: [], status: 'banned' });
			group.requests = group.requests.filter((x) => x.user !== user);
			if (was) { if (small(group)) note(group, ME, `member_removed:${user}`); else info(); }
			if (body.delete_messages) {
				const seqs = group.messages.filter((x) => x.sender === user && !x.deleted && x.kind !== 'system').map((x) => x.seq);
				if (seqs.length) deleteMany(group, seqs);
			}
			return json(res, 204);
		}
		if (what === '/ban' && m === 'DELETE') {
			if (need(rights.includes('ban_users'))) return;
			if (t && t.status === 'banned') t.status = 'kicked';
			return json(res, 204);
		}
	}
	if (rest === '/leave' && m === 'POST') {
		if (me.role === 'owner' && count(group) > 1) return problem(res, 409, 'group.owner_must_transfer', 'پیش از ترک گروه مالکیت را واگذار کن');
		me.status = 'left';
		if (small(group)) note(group, ME, 'member_left'); else info();
		return json(res, 204);
	}
	if (rest === '/transfer' && m === 'POST') {
		if (need(me.role === 'owner')) return;
		const t = target(body.user);
		if (!t || t.status !== 'member') return problem(res, 409, 'group.transfer_target', 'این کاربر عضو گروه نیست');
		me.role = 'member'; me.rights = [];
		t.role = 'owner'; t.rights = [...ALL_RIGHTS];
		info();
		return json(res, 200, groupView(group));
	}
	if (rest === '/member' && m === 'PATCH') {
		if (body.muted !== undefined) { group.me.muted = !!body.muted; group.me.muted_until = 0; }
		if (body.mute_for !== undefined) {
			const hours = { '1h': 1, '8h': 8, '2d': 48 }[body.mute_for];
			if (body.mute_for !== 'off' && body.mute_for !== 'forever' && !hours) return problem(res, 400, 'chat.mute_duration', 'مدت بی‌صدا درست نیست');
			group.me.muted = body.mute_for !== 'off';
			group.me.muted_until = hours ? Math.floor(Date.now() / 1000) + hours * 3600 : 0;
		}
		if (body.notify !== undefined) {
			if (!['all', 'mentions', 'none'].includes(body.notify)) return problem(res, 400, 'group.notify_invalid', 'سطح اعلان درست نیست');
			group.me.notify = body.notify;
		}
		if (body.archived !== undefined) group.me.archived = !!body.archived;
		if (body.clear_history) { group.me.cleared_before_seq = group.last_seq; group.me.last_read_seq = group.last_seq; }
		if (body.hide_seq) group.me.hidden.add(Number(body.hide_seq));
		return json(res, 204);
	}
	if (rest === '/pin') { group.me.pinned = (m === 'POST'); return json(res, 204); }
	if (rest === '/read' && m === 'POST') {
		group.me.last_read_seq = Math.max(group.me.last_read_seq, Math.min(Number(body.seq) || 0, group.last_seq));
		return json(res, 200, { last_read_seq: group.me.last_read_seq });
	}
	if (rest === '/difference' && m === 'GET') {
		const from = Math.min(Number(q.get('pts')) || 0, group.pts);
		const changed = group.messages.filter((x) => x._pts > from && visible(group, x)).sort((a, b) => b._pts - a._pts);
		if (changed.length > 200) return json(res, 200, { pts: group.pts, messages: [], group: groupView(group), more: false, too_long: true });
		const out = { pts: group.pts, messages: changed.map((x) => messageView(group, x)), more: false, too_long: false };
		if (group.pts > from) out.group = groupView(group);
		return json(res, 200, out);
	}
	if (rest === '/messages' && m === 'GET') {
		const limit = Math.min(Number(q.get('limit')) || 50, 50);
		const all = group.messages.filter((x) => visible(group, x));
		const before = Number(q.get('before')) || 0, after = Number(q.get('after')) || 0, around = Number(q.get('around')) || 0;
		let list;
		if (after) list = all.filter((x) => x.seq > after).slice(0, limit);
		else if (around) { const at = Math.max(0, all.findIndex((x) => x.seq >= around) - Math.floor(limit / 2)); list = all.slice(at, at + limit); }
		else if (before) list = all.filter((x) => x.seq < before).slice(-limit);
		else list = all.slice(-limit);
		const first = list[0]?.seq ?? 0, last = list.at(-1)?.seq ?? 0;
		return json(res, 200, {
			group: groupView(group),
			messages: list.map((x) => messageView(group, x)),
			has_more_before: all.some((x) => x.seq < first),
			has_more_after: all.some((x) => x.seq > last),
		});
	}
	if (rest === '/messages' && m === 'POST') {
		const known = group.messages.find((x) => x.sender === ME && x.client_message_id === body.client_message_id);
		if (known) return json(res, 200, { message_id: known.id, seq: known.seq, pts: group.pts, created_at: known.created_at, duplicate: true });
		for (const key of Object.keys(body)) {
			// Strict bodies: what the route does not know is refused.
			if (!['client_message_id', 'kind', 'text', 'reply_to_seq', 'quote', 'entities', 'link_preview', 'attachment', 'media_spoiler', 'sticker', 'forwarded_post', 'location', 'audio', 'forward_from', 'silent'].includes(key)) {
				return problem(res, 400, 'request.invalid', `unknown field ${key}`);
			}
		}
		const kind = body.kind || 'text';
		const media = !['text', 'forwarded_post'].includes(kind);
		if (!permissions.includes('send_messages') || (media && !permissions.includes('send_media')) || String(body.text || '').startsWith('fail')) {
			return problem(res, 403, 'group.send_restricted', 'نمی‌توانی اینجا پیام بفرستی');
		}
		if (group.slow && me.role === 'member') {
			if (group.slow.until > Date.now()) {
				const wait = Math.ceil((group.slow.until - Date.now()) / 1000);
				res.setHeader('Retry-After', String(wait));
				return problem(res, 429, 'group.slow_mode', 'حالت آهسته: کمی صبر کن', { retry_after: wait, retryable: true });
			}
			group.slow.until = Date.now() + group.slow.seconds * 1000;
		}
		const extra = { client_message_id: body.client_message_id };
		for (const key of ['text', 'reply_to_seq', 'quote', 'entities', 'attachment', 'media_spoiler', 'forwarded_post', 'location', 'audio']) {
			if (body[key] !== undefined && body[key] !== '' && body[key] !== 0) extra[key] = body[key];
		}
		if (body.forward_from) extra.forwarded = true;
		if (body.sticker) extra.sticker = { id: body.sticker, pack: 'p1', emoji: '🙂', url: `/media/sticker/${String(body.sticker).replace(/\D/g, '') || 1}.png`, width: 256, height: 256 };
		if (body.attachment) extra.media = { width: 960, height: 640, mime: 'image/png', size: 52000 };
		const message = post(group, ME, kind, extra);
		group.me.draft = null;
		answer(group, message);
		return json(res, 200, { message_id: message.id, seq: message.seq, pts: group.pts, created_at: message.created_at });
	}
	if (rest === '/messages/delete' && m === 'POST') {
		if (!PHASE2) return problem(res, 404, 'not_found', 'not found');
		const seqs = (body.seqs || []).map(Number);
		if (!seqs.length || seqs.length > 100) return problem(res, 400, 'request.invalid', 'درخواست نادرست');
		const found = group.messages.filter((x) => seqs.includes(x.seq) && !x.deleted && x.kind !== 'system' && !x.under_review);
		if (!rights.includes('delete_messages') && found.some((x) => x.sender !== ME)) return problem(res, 403, 'message.not_sender', 'این پیام مال تو نیست');
		return json(res, 200, { deleted: deleteMany(group, found.map((x) => x.seq)) });
	}
	if ((match = rest.match(/^\/messages\/(\d+)(\/reaction|\/attachment|\/pin)?$/))) {
		const seq = Number(match[1]), what = match[2] || '';
		const message = group.messages.find((x) => x.seq === seq && visible(group, x));
		if (!message) return problem(res, 404, 'message.not_found', 'پیام پیدا نشد');
		if (what === '/attachment') {
			stats.attachments++;
			const [w, h] = [message.media?.width || 640, message.media?.height || 480];
			const scale = Math.min(1, (q.get('variant') === 'thumb' ? 320 : 960) / Math.max(w, h));
			return sendImage(res, picture(`${group.id}:${seq}`, Math.max(8, Math.round(w * scale)), Math.max(8, Math.round(h * scale))));
		}
		if (what === '/reaction') {
			const was = group.me.reactions[seq];
			const next = (m === 'PUT') ? String(body.emoji || '') : '';
			if (was === next || (!was && !next)) return json(res, 204);
			message.reactions = { ...(message.reactions || {}) };
			if (was) { message.reactions[was] = (message.reactions[was] || 1) - 1; if (message.reactions[was] <= 0) delete message.reactions[was]; }
			if (next) message.reactions[next] = (message.reactions[next] || 0) + 1;
			if (!Object.keys(message.reactions).length) delete message.reactions;
			if (next) group.me.reactions[seq] = next; else delete group.me.reactions[seq];
			touch(group, message);
			return json(res, 204);
		}
		if (what === '/pin') {
			if (!PHASE2) return problem(res, 404, 'not_found', 'not found');
			if (need(permissions.includes('pin_messages'))) return;
			if (message.deleted || message.under_review || message.kind === 'system') return problem(res, 404, 'message.not_found', 'پیام پیدا نشد');
			const pinned = group.pins.some((x) => x.seq === seq);
			if (m === 'POST' && !pinned) { group.pins.unshift({ seq, pinned_by: ME, pinned_at: Date.now() }); note(group, ME, `pinned:${seq}`); }
			if (m === 'DELETE' && pinned) { group.pins = group.pins.filter((x) => x.seq !== seq); ++group.pts; }
			return json(res, 204);
		}
		if (m === 'PATCH') {
			if (message.sender !== ME) return problem(res, 403, 'message.not_sender', 'این پیام مال تو نیست');
			message.text = String(body.text || '');
			message.entities = body.entities || [];
			if (!message.entities.length) delete message.entities;
			message.edited = true;
			touch(group, message);
			return json(res, 204);
		}
		if (m === 'DELETE') {
			if (q.get('scope') === 'everyone') {
				if (message.sender !== ME && !rights.includes('delete_messages')) return problem(res, 403, 'group.right_required', 'اجازهٔ این کار را نداری');
				message.deleted = true;
				group.pins = group.pins.filter((x) => x.seq !== seq);
				touch(group, message);
			} else {
				group.me.hidden.add(seq);
			}
			return json(res, 204);
		}
	}
	if (!PHASE2) return problem(res, 404, 'not_found', 'not found');
	if (rest === '/pins' && m === 'GET') {
		return json(res, 200, { pins: group.pins.map((pin) => {
			const message = group.messages.find((x) => x.seq === pin.seq && visible(group, x));
			return message ? { ...pin, message: messageView(group, message) } : { ...pin };
		}) });
	}
	if (rest === '/mentions' && m === 'GET') return json(res, 200, { seqs: mentionsOf(group).slice(0, 100) });
	if (rest === '/draft') {
		if (m === 'PUT' && String(body.text || '').trim()) {
			group.me.draft = { text: body.text, updated_at: Date.now(), ...(body.reply_to_seq ? { reply_to_seq: body.reply_to_seq } : {}), ...(body.entities?.length ? { entities: body.entities } : {}) };
			return json(res, 200, { updated_at: group.me.draft.updated_at });
		}
		group.me.draft = null;
		return m === 'PUT' ? json(res, 200, { updated_at: Date.now() }) : json(res, 204);
	}
	if (rest === '/invites' && m === 'GET') {
		if (need(rights.includes('invite_users'))) return;
		return json(res, 200, { invites: group.invites.filter((x) => !x.revoked).map((x) => inviteView(group, x)) });
	}
	if (rest === '/invites' && m === 'POST') {
		if (need(rights.includes('invite_users'))) return;
		if ([...String(body.name || '')].length > 32 || body.expires_in < 0 || body.max_uses < 0) return problem(res, 400, 'group.invite_params_invalid', 'مشخصات پیوند درست نیست');
		if (group.invites.filter((x) => !x.revoked).length >= 100) return problem(res, 409, 'group.invite_limit', 'پیوندهای گروه کامل است');
		const invite = addInvite(group, {
			...(body.name ? { name: String(body.name) } : {}),
			...(body.requires_approval ? { requires_approval: true } : {}),
			...(body.expires_in ? { expires_at: Math.floor(Date.now() / 1000) + Number(body.expires_in) } : {}),
			...(body.max_uses ? { max_uses: Number(body.max_uses) } : {}),
		});
		return json(res, 201, inviteView(group, invite));
	}
	if ((match = rest.match(/^\/invites\/([^/]+)$/)) && m === 'DELETE') {
		if (need(rights.includes('invite_users'))) return;
		const invite = group.invites.find((x) => x.link === decodeURIComponent(match[1]));
		if (!invite) return problem(res, 404, 'group.invite_invalid', 'این پیوند دیگر معتبر نیست');
		invite.revoked = true;
		return json(res, 204);
	}
	if (rest === '/join-requests' && m === 'GET') {
		if (need(rights.includes('ban_users'))) return;
		const { items, next } = page(group.requests, q.get('cursor'), Math.min(Number(q.get('limit')) || 50, 100));
		return json(res, 200, next ? { requests: items, next_cursor: next } : { requests: items });
	}
	if ((match = rest.match(/^\/join-requests\/([^/]+)\/(approve|decline)$/)) && m === 'POST') {
		if (need(rights.includes('ban_users'))) return;
		const user = decodeURIComponent(match[1]);
		if (!group.requests.some((x) => x.user === user)) return problem(res, 404, 'group.join_request_not_found', 'این درخواست دیگر نیست');
		group.requests = group.requests.filter((x) => x.user !== user);
		if (match[2] === 'approve') {
			const t = target(user);
			if (t) Object.assign(t, { status: 'member', role: 'member', rights: [] }); else join(group, user, 'member', { joined_at: Date.now() });
			if (small(group)) note(group, user, 'member_joined'); else info();
		}
		return json(res, 204);
	}
	return problem(res, 404, 'not_found', 'not found');
}

// One change for several messages: they all carry the same pts.
function deleteMany(group, seqs) {
	const list = [...new Set(seqs)].sort((a, b) => a - b);
	if (!list.length) return [];
	const pts = ++group.pts;
	for (const message of group.messages) {
		if (list.includes(message.seq)) { message.deleted = true; message._pts = pts; }
	}
	group.pins = group.pins.filter((x) => !list.includes(x.seq));
	return list;
}

const server = http.createServer(async (req, res) => {
	const url = new URL(req.url, `http://127.0.0.1:${PORT}`);
	const p = url.pathname, m = req.method;
	const raw = await readBody(req);
	let match;

	if (p === '/control/stats') return json(res, 200, stats);
	if (p === '/control/frame' && m === 'POST') {
		const body = parse(raw);
		const group = groups[body.group];
		if (!group) return json(res, 404, {});
		const message = post(group, body.sender || 'u_sara', 'text', { text: body.text || 'سلام' });
		return json(res, 200, { seq: message.seq, pts: group.pts });
	}
	if (p === '/control/kick' && m === 'POST') {
		const group = groups[parse(raw).group];
		const me = group && memberOf(group, ME);
		if (me) { me.status = 'kicked'; note(group, 'u_sara', `member_removed:${ME}`); }
		return json(res, 200, {});
	}

	if (p === '/v1/client-config') {
		return forward(req, res, raw, (config) => ({ version: 431, values: configValues(config.values || config) }));
	}
	// People: every card has a picture, and the crowd of the large group exists.
	if ((match = p.match(/^\/v1\/users\/([^/]+)\/avatar$/))) {
		const id = decodeURIComponent(match[1]);
		stats.avatars[id] = (stats.avatars[id] || 0) + 1;
		return sendImage(res, face(id, url.searchParams.get('variant') === 'thumb' ? 160 : 480));
	}
	if (p === '/v1/users' && m === 'GET') {
		const ids = (url.searchParams.get('ids') || '').split(',').filter(Boolean);
		return forward(req, res, raw, (answer) => {
			const known = new Map((answer.users || []).map((user) => [user.id, user]));
			return { ...answer, users: ids.map((id) => {
				const base = known.get(id) || people[id];
				if (!base) return null;
				return (base.deleted || !people[id]) ? base : { ...base, avatar_id: base.avatar_id || `av_${id}` };
			}).filter(Boolean) };
		});
	}
	if (p === '/v1/search' && url.searchParams.get('group')) {
		const group = groups[url.searchParams.get('group')];
		if (url.searchParams.get('type') !== 'users') return problem(res, 400, 'search.query_invalid', 'جستجو درست نیست');
		const text = (url.searchParams.get('q') || '').toLowerCase().replace(/^@/, '');
		const users = (group && memberOf(group, ME)) ? group.members
			.filter((x) => x.status === 'member' && people[x.user] && (`${people[x.user].display_name} ${people[x.user].handle || ''}`).toLowerCase().includes(text))
			.slice(0, 20)
			.map((x) => ({ ...card(x.user), follow_state: 'none', follows_you: false, group_role: x.role })) : [];
		return json(res, 200, { users });
	}
	if (p === '/v1/reports' && m === 'POST') {
		const body = parse(raw);
		if (['group', 'group_message'].includes(body.target_kind)) {
			const group = groups[body.target];
			if (!group || !memberOf(group, ME)) return problem(res, 404, 'report.target_not_found', 'چیزی برای گزارش پیدا نشد');
			if (body.target_kind === 'group_message' && !(Number(body.message_seq) > 0)) return problem(res, 404, 'report.target_not_found', 'چیزی برای گزارش پیدا نشد');
			return json(res, 201, { report: 'r1mock', status: 'open' });
		}
	}
	if (p === '/v1/groups' || p.startsWith('/v1/groups/') || p.startsWith('/v1/group-invites/')) {
		try {
			return await handleGroups(req, res, url, raw);
		} catch (error) {
			console.error(error);
			return problem(res, 500, 'mock.failed', String(error));
		}
	}
	// The cards of the same people in whatever the mocks behind answer (lists of
	// friends, search) carry the same picture: one person, one avatar id.
	return forward(req, res, raw, withFaces);
});

server.listen(PORT, '127.0.0.1', () => {
	console.log(`mock groups on http://127.0.0.1:${PORT} -> ${TARGET.origin} (groups ${GROUPS_ON ? (PHASE2 ? 'on, both releases' : 'on, first release') : 'off'}, video calls ${VIDEO_ON ? 'on' : 'off'})`);
});
