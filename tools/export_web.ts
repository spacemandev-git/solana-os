#!/usr/bin/env bun
/*
 * Publish the badge build (firmware/build, from `idf.py build`) for the site's /stock
 * flasher: copies the files idf.py flashes into web/static/firmware/solana-os/ and
 * writes web/static/firmware/solana-os.json. Nothing here is hand-maintained: offsets,
 * flash settings and the NVS range come from the build, the version from the app image.
 *   bun firmware/tools/export_web.ts        (tools/build_web.sh runs it)
 */
import { createHash } from 'node:crypto';
import { mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import type { FirmwareManifest } from '../../web/src/lib/firmware';

const root = resolve(import.meta.dir, '../..');
const build = join(root, 'firmware/build');
const outDir = join(root, 'web/static/firmware');
const ID = 'solana-os';

const args = JSON.parse(readFileSync(join(build, 'flasher_args.json'), 'utf8'));
const cstr = (b: Uint8Array) => new TextDecoder().decode(b.subarray(0, b.indexOf(0) < 0 ? b.length : b.indexOf(0)));
// esptool-js pads each image to 4 bytes before it hashes the flash, so hash the padded bytes.
const md5 = (b: Uint8Array) => {
	const padded = new Uint8Array(Math.ceil(b.length / 4) * 4).fill(0xff);
	padded.set(b);
	return createHash('md5').update(padded).digest('hex');
};
const sha256 = (b: Uint8Array) => createHash('sha256').update(b).digest('hex');

// esp_app_desc_t (version @16, time @80, date @96, idf_ver @112) sits right after the image header (24 B) and first segment header (8 B).
const app = readFileSync(join(build, args.app.file));
const desc = app.subarray(32, 32 + 256);
if (new DataView(desc.buffer, desc.byteOffset).getUint32(0, true) !== 0xabcd5432) {
	throw new Error(`${args.app.file}: no esp_app_desc_t`);
}

// Partition table: 32-byte entries (magic 0x50AA, type, subtype, offset, size, label).
const table = readFileSync(join(build, args['partition-table'].file));
let nvs: { offset: number; size: number } | undefined;
for (let i = 0; i + 32 <= table.length; i += 32) {
	const e = new DataView(table.buffer, table.byteOffset + i, 32);
	if (e.getUint16(0, true) !== 0x50aa) break;
	if (e.getUint8(2) === 1 && e.getUint8(3) === 2) nvs = { offset: e.getUint32(4, true), size: e.getUint32(8, true) };
}
if (!nvs) throw new Error('partition table has no NVS partition');

rmSync(outDir, { recursive: true, force: true });
mkdirSync(join(outDir, ID), { recursive: true });

const parts = Object.entries(args.flash_files as Record<string, string>)
	.map(([offset, file]) => ({ offset: Number(offset), file }))
	.sort((a, b) => a.offset - b.offset)
	.map(({ offset, file }) => {
		const data = readFileSync(join(build, file));
		const name = file.split('/').pop()!;
		writeFileSync(join(outDir, ID, name), data);
		return { name, offset, file: `${ID}/${name}`, size: data.length, sha256: sha256(data), md5: md5(data) };
	});

// "-dirty": uncommitted changes to what goes into the badge build (not tools/ or emulator/).
const git = (...a: string[]) => Bun.spawnSync(['git', '-C', root, ...a]).stdout.toString().trim();
const manifest: FirmwareManifest = {
	id: ID,
	name: 'Solana OS',
	chip: args.extra_esptool_args.chip.toUpperCase().replace(/^ESP32/, 'ESP32-'),
	version: cstr(desc.subarray(16, 48)),
	built: `${cstr(desc.subarray(96, 112))} ${cstr(desc.subarray(80, 96))}`,
	idf: cstr(desc.subarray(112, 144)),
	commit: git('rev-parse', '--short', 'HEAD') + (git('status', '--porcelain', '--', 'firmware', ':!firmware/tools', ':!firmware/emulator') ? '-dirty' : ''),
	flash: args.flash_settings,
	parts,
	nvs: { ...nvs, md5: md5(new Uint8Array(nvs.size).fill(0xff)) }
};
writeFileSync(join(outDir, `${ID}.json`), JSON.stringify(manifest, null, '\t') + '\n');
console.log(
	`${manifest.name} ${manifest.version} (${manifest.commit}, ${manifest.built}) -> web/static/firmware/\n` +
		parts.map((p) => `  0x${p.offset.toString(16).padStart(5, '0')}  ${p.file}  ${p.size} B`).join('\n')
);
