// Builds and starts the realtime test peer.
//
//   ELEZA_PWA_DIR=<checkout of the web client> [ELEZA_RTPEER_WORK=<scratch dir>] node run.mjs
//
// The peer's protocol layer (Noise NK, record framing, the protobuf codec and its schema) is
// the web client's own src/rt/*.ts, bundled from ELEZA_PWA_DIR - nothing is shared with the
// C++ implementation it is tested against. Dependencies are installed into the work
// directory on first use.
//
// Exit code 77 = a prerequisite is missing (the test is skipped); the reason goes to stderr.
import { spawnSync } from 'node:child_process';
import { copyFileSync, existsSync, mkdirSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const pwa = process.env.ELEZA_PWA_DIR || path.resolve(here, '../../../../pwa');
const work = process.env.ELEZA_RTPEER_WORK || path.join(here, '.work');

function skip(reason) {
  console.error('rtpeer: skipped: ' + reason);
  process.exit(77);
}

const sources = path.join(pwa, 'src', 'rt');
for (const name of ['noise.ts', 'frames.ts', 'proto.ts', 'schema.ts']) {
  if (!existsSync(path.join(sources, name))) skip('no web client sources at ' + sources);
}

mkdirSync(work, { recursive: true });
const modules = path.join(work, 'node_modules');
const needed = ['esbuild', 'ws', '@noble/curves', '@noble/ciphers', '@noble/hashes'];
if (!needed.every((name) => existsSync(path.join(modules, name, 'package.json')))) {
  copyFileSync(path.join(here, 'package.json'), path.join(work, 'package.json'));
  const npm = process.platform === 'win32' ? 'npm.cmd' : 'npm';
  const result = spawnSync(npm, ['install', '--no-audit', '--no-fund', '--loglevel=error'], {
    cwd: work,
    stdio: ['ignore', 'ignore', 'inherit'],
    shell: process.platform === 'win32',
  });
  if (result.error || result.status !== 0) skip('npm install failed (no network?)');
}

const bundle = path.join(work, 'peer.bundle.cjs');
try {
  const esbuild = await import(pathToFileURL(path.join(modules, 'esbuild', 'lib', 'main.js')).href);
  await esbuild.build({
    entryPoints: [path.join(here, 'peer.ts')],
    outfile: bundle,
    bundle: true,
    platform: 'node',
    format: 'cjs',
    target: 'node18',
    alias: { '@pwa-rt': sources },
    nodePaths: [modules],
    external: ['bufferutil', 'utf-8-validate'],
    logLevel: 'error',
  });
} catch (error) {
  skip('bundling failed: ' + (error && error.message ? error.message : error));
}

await import(pathToFileURL(bundle).href);
