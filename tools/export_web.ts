#!/usr/bin/env bun
/*
 * Publish the badge build (firmware/build, from `idf.py build`) for the site's /stock
 * flasher: the files idf.py flashes go to web/static/firmware/solana-os/, the manifest
 * to web/static/firmware/solana-os.json (export_lib.ts).
 *   bun firmware/tools/export_web.ts        (tools/build_web.sh runs it)
 */
import { writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { describe, exportBuild, outRoot, root } from './export_lib';

const ID = 'solana-os';
// "-dirty": uncommitted changes to what goes into the badge build (not tools/ or emulator/).
const manifest = exportBuild({
	build: join(root, 'firmware/build'),
	id: ID,
	name: 'Solana OS',
	dir: ID,
	sources: ['firmware', ':!firmware/tools', ':!firmware/emulator']
});
writeFileSync(join(outRoot, `${ID}.json`), JSON.stringify(manifest, null, '\t') + '\n');
console.log(describe(manifest, 'web/static/firmware/'));
