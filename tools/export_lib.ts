/*
 * Publishes an ESP-IDF build for the site's Web Serial flashers: copies the files
 * `idf.py flash` writes into web/static/firmware/<dir>/ and returns their manifest.
 * Nothing is hand-maintained: offsets, flash settings and the NVS range come from the
 * build, the version from the app image. Used by firmware/tools/export_web.ts (Solana OS,
 * /stock) and beacon/tools/export_web.ts (quest beacons, /admin/beacons/flash).
 */
import { createHash } from 'node:crypto';
import { mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import type { FirmwareManifest } from '../../web/src/lib/firmware';

export const root = resolve(import.meta.dir, '../..');
export const outRoot = join(root, 'web/static/firmware');

const cstr = (b: Uint8Array) => new TextDecoder().decode(b.subarray(0, b.indexOf(0) < 0 ? b.length : b.indexOf(0)));
// esptool-js pads each image to 4 bytes before it hashes the flash, so hash the padded bytes.
export const md5 = (b: Uint8Array) => {
	const padded = new Uint8Array(Math.ceil(b.length / 4) * 4).fill(0xff);
	padded.set(b);
	return createHash('md5').update(padded).digest('hex');
};
const sha256 = (b: Uint8Array) => createHash('sha256').update(b).digest('hex');

/** esptool's target ("esp32c5", "esp32") as esptool-js names the chip ("ESP32-C5", "ESP32"). */
export const chipName = (target: string) => target.toUpperCase().replace(/^ESP32(?=.)/, 'ESP32-');

export function exportBuild(opts: {
	/** The idf.py build directory (has flasher_args.json). */
	build: string;
	id: string;
	name: string;
	/** Directory under web/static/firmware/ for the images, emptied first. */
	dir: string;
	/** Paths whose uncommitted changes mark the build "-dirty" (git pathspecs). */
	sources: string[];
}): FirmwareManifest {
	const { build } = opts;
	const args = JSON.parse(readFileSync(join(build, 'flasher_args.json'), 'utf8'));

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
	if (!nvs) throw new Error(`${build}: the partition table has no NVS partition`);

	const out = join(outRoot, opts.dir);
	rmSync(out, { recursive: true, force: true });
	mkdirSync(out, { recursive: true });

	const parts = Object.entries(args.flash_files as Record<string, string>)
		.map(([offset, file]) => ({ offset: Number(offset), file }))
		.sort((a, b) => a.offset - b.offset)
		.map(({ offset, file }) => {
			const data = readFileSync(join(build, file));
			const name = file.split('/').pop()!;
			writeFileSync(join(out, name), data);
			return { name, offset, file: `${opts.dir}/${name}`, size: data.length, sha256: sha256(data), md5: md5(data) };
		});

	const git = (...a: string[]) => Bun.spawnSync(['git', '-C', root, ...a]).stdout.toString().trim();
	return {
		id: opts.id,
		name: opts.name,
		chip: chipName(args.extra_esptool_args.chip),
		version: cstr(desc.subarray(16, 48)),
		built: `${cstr(desc.subarray(96, 112))} ${cstr(desc.subarray(80, 96))}`,
		idf: cstr(desc.subarray(112, 144)),
		commit: git('rev-parse', '--short', 'HEAD') + (git('status', '--porcelain', '--', ...opts.sources) ? '-dirty' : ''),
		flash: args.flash_settings,
		parts,
		nvs: { ...nvs, md5: md5(new Uint8Array(nvs.size).fill(0xff)) }
	};
}

export const describe = (m: FirmwareManifest, to: string) =>
	`${m.name} ${m.version} for the ${m.chip} (${m.commit}, ${m.built}) -> ${to}\n` +
	m.parts.map((p) => `  0x${p.offset.toString(16).padStart(5, '0')}  ${p.file}  ${p.size} B`).join('\n');
