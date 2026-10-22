#!/usr/bin/env node
// A proxy in front of the chat mock (tests/tools/mock-history.mjs, which itself
// fronts the web client's mock API) that adds sending a story to people
// (backend docs/stories.md §7) and one story by id (§6), which the web mock does
// not have:
//
//   * u_ali's photo stA1 carries the post p1, as a story made from a post does;
//   * every Story gets can_share (not for close-friends stories, nor for
//     u_ali's, who turned sharing off) and, on my own, share_count;
//   * GET /v1/stories/{id}: the story and its owner, 404 story.not_found for a
//     story that is gone or that the chat's fixtures say I may not open (stN1);
//   * POST /v1/stories/{id}/share {to, groups, text}: u_nika answers not_allowed,
//     u_bot unavailable, every group limit, everybody else is sent (my own id
//     too: Saved Messages); 403 story.share_not_allowed where can_share is false;
//   * the story settings carry allow_sharing (PATCH changes it), the viewers
//     list carries shares. GET /_mock/shares lists what was sent;
//   * while I have no story of my own, one photo (stMine, sara's picture) so that
//     «N بیننده · N ارسال» and «انتشار در الی» can be seen.
//
//   PORT=8102 node tests/tools/mock-history.mjs &
//   PORT=8105 TARGET=http://127.0.0.1:8102 node tests/tools/mock-stories-proxy.mjs &
//   Eleza --api=http://127.0.0.1:8105 --debug-login=+989121234567:123456 --debug-route=chat/stories
import http from 'node:http';

const PORT = Number(process.env.PORT || 8105);
const TARGET = new URL(process.env.TARGET || 'http://127.0.0.1:8102');
const OWNERS = ['u_sara', 'u_reza', 'u_ali', 'u_nika', 'u_me'];
const NOT_SHARED = new Set(['u_ali']); // allow_sharing: false
const HIDDEN = new Set(['stN1']); // not mine to open
const shares = []; // { story, to, groups, text }
const shareCounts = {}; // story -> messages it became
let allowSharing = true;

const json = (res, status, body) => {
	res.writeHead(status, { 'Content-Type': status >= 400 ? 'application/problem+json' : 'application/json' });
	res.end(JSON.stringify(body));
};

function upstream(method, path, headers, body, done) {
	const request = http.request({
		host: TARGET.hostname,
		port: TARGET.port,
		method,
		path,
		headers: { ...headers, host: TARGET.host },
	}, (answer) => {
		const chunks = [];
		answer.on('data', (chunk) => chunks.push(chunk));
		answer.on('end', () => done(answer, Buffer.concat(chunks)));
	});
	request.on('error', () => done(null, null));
	request.end(body);
}

function forward(req, res, body, rewrite) {
	upstream(req.method, req.url, req.headers, body, (answer, data) => {
		if (!answer) {
			return json(res, 502, { code: 'proxy.unreachable', title: 'mock api is not running' });
		}
		const headers = { ...answer.headers };
		if (rewrite && answer.statusCode < 300 && (headers['content-type'] || '').includes('json')) {
			try {
				data = Buffer.from(JSON.stringify(rewrite(JSON.parse(data.toString('utf8')))));
			} catch { /* not json after all */ }
		}
		delete headers['content-length'];
		res.writeHead(answer.statusCode, headers);
		res.end(data);
	});
}

const nowS = () => Math.floor(Date.now() / 1000);
const mine = () => ({
	id: 'stMine', kind: 'image',
	media: { url: '/v1/stories/stMine/media?variant=full', thumb_url: '/v1/stories/stMine/media?variant=thumb', width: 1080, height: 1920, duration_ms: 5000 },
	post: null, caption: 'یک روز خوب', created_at: nowS() - 600, expires_at: nowS() + 80000, seen: true, my_reaction: null,
	viewer_count: 12, audience: 'everyone', can_reply: false,
});

// stA1 is a photo made from the Eli post p1 (media_id + post_id): «دیدن الی».
const decorate = (story, owner) => ({
	...story,
	...(story.id === 'stA1' ? { post: { id: 'p1', author: { id: 'u_sara', handle: 'sara', display_name: 'سارا احمدی' }, text_excerpt: '', thumb_url: null, available: true } } : {}),
	can_share: story.audience !== 'close_friends' && (owner === 'u_me' || (allowSharing && !NOT_SHARED.has(owner))),
	...(owner === 'u_me' ? { share_count: shareCounts[story.id] || 0 } : {}),
});

// Finds a story by reading its owner's list (the web mock has no route by id).
function findStory(req, id, done) {
	let left = OWNERS.length;
	let found = null;
	const headers = { ...req.headers };
	delete headers['content-length'];
	delete headers['content-type'];
	for (const owner of OWNERS) {
		upstream('GET', `/v1/users/${owner}/stories`, headers, null, (answer, data) => {
			if (answer && answer.statusCode === 200) {
				try {
					const page = JSON.parse(data.toString('utf8'));
					const stories = page.stories || [];
					if (owner === 'u_me' && !stories.length) stories.push(mine());
					const story = stories.find((x) => x.id === id);
					if (story) found = { user: page.user, story: decorate(story, owner), owner };
				} catch { /* */ }
			}
			if (--left === 0) done(found);
		});
	}
}

http.createServer((req, res) => {
	const chunks = [];
	req.on('data', (chunk) => chunks.push(chunk));
	req.on('end', () => {
		const body = Buffer.concat(chunks);
		const url = new URL(req.url, 'http://x');
		const p = url.pathname;
		const m = req.method;
		let mm;
		if (p === '/_mock/shares') {
			return json(res, 200, { shares, allow_sharing: allowSharing });
		}
		if ((mm = p.match(/^\/v1\/users\/([^/]+)\/stories$/)) && m === 'GET') {
			const owner = decodeURIComponent(mm[1]);
			return forward(req, res, body, (page) => {
				const stories = page.stories || [];
				if (owner === 'u_me' && !stories.length) stories.push(mine());
				return { ...page, stories: stories.map((x) => decorate(x, owner)) };
			});
		}
		if ((mm = p.match(/^\/v1\/stories\/stMine\/(media|viewers)$/))) {
			if (mm[1] === 'viewers') return json(res, 200, { viewers: [], next_cursor: null, total: 12, shares: shareCounts.stMine || 0 });
			req.url = req.url.replace('/stMine/', '/stS1/');
		}
		if ((mm = p.match(/^\/v1\/stories\/([^/]+)$/)) && m === 'GET' && mm[1] !== 'tray') {
			const id = decodeURIComponent(mm[1]);
			if (HIDDEN.has(id)) return json(res, 404, { code: 'story.not_found', title: 'not found' });
			return findStory(req, id, (found) => found
				? json(res, 200, { user: found.user, story: found.story })
				: json(res, 404, { code: 'story.not_found', title: 'not found' }));
		}
		if ((mm = p.match(/^\/v1\/stories\/([^/]+)\/share$/)) && m === 'POST') {
			const id = decodeURIComponent(mm[1]);
			let input = {};
			try { input = JSON.parse(body.toString('utf8') || '{}'); } catch { /* */ }
			const to = input.to || [];
			const groups = input.groups || [];
			if (!to.length && !groups.length) return json(res, 400, { code: 'story.invalid', title: 'no recipient' });
			if (to.length + groups.length > 15) return json(res, 400, { code: 'story.invalid', title: 'too many' });
			return findStory(req, id, (found) => {
				if (!found) return json(res, 404, { code: 'story.not_found', title: 'not found' });
				if (!found.story.can_share) return json(res, 403, { code: 'story.share_not_allowed', title: 'not allowed' });
				const sent = [];
				const notSent = [];
				let seq = 50 + shares.length * 20;
				for (const user of to) {
					if (user === 'u_nika') notSent.push({ to: user, reason: 'not_allowed' });
					else if (user === 'u_bot') notSent.push({ to: user, reason: 'unavailable' });
					else sent.push({ to: user, chat: user === 'u_me' ? 'saved' : 'c_' + user, seq: ++seq });
				}
				for (const group of groups) notSent.push({ to: group, reason: 'limit' });
				shares.push({ story: id, to, groups, text: input.text || '' });
				shareCounts[id] = (shareCounts[id] || 0) + sent.length;
				return json(res, 200, { sent, not_sent: notSent });
			});
		}
		if ((mm = p.match(/^\/v1\/stories\/([^/]+)\/viewers$/)) && m === 'GET') {
			const id = decodeURIComponent(mm[1]);
			return forward(req, res, body, (page) => ({ ...page, shares: shareCounts[id] || 0 }));
		}
		if (p === '/v1/me/story-settings') {
			if (m === 'PATCH') {
				try {
					const input = JSON.parse(body.toString('utf8') || '{}');
					if (typeof input.allow_sharing === 'boolean') allowSharing = input.allow_sharing;
				} catch { /* */ }
			}
			return forward(req, res, body, (settings) => ({ ...settings, allow_sharing: allowSharing }));
		}
		forward(req, res, body);
	});
}).listen(PORT, '127.0.0.1', () => console.log(`mock stories proxy on http://127.0.0.1:${PORT} -> ${TARGET.origin}`));
