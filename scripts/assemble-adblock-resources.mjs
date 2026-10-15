#!/usr/bin/env node
// Copyright 2026. SPDX-License-Identifier: MPL-2.0
// Offline adaptation of Brave's adBlockRustUtils.js resource assembler.
// Inputs must be the commits recorded in resources/SOURCES.json, never user JS.
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { createHash } from 'node:crypto';
const [uboRoot, braveRoot, output] = process.argv.slice(2).map(p => path.resolve(p));
const { default: redirects } = await import(pathToFileURL(path.join(uboRoot, 'src/js/redirect-resources.js')));
const { builtinScriptlets } = await import(pathToFileURL(path.join(uboRoot, 'src/js/resources/scriptlets.js')));
const mime = { '.js':'application/javascript', '.gif':'image/gif', '.png':'image/png', '.mp3':'audio/mp3', '.mp4':'video/mp4', '.html':'text/html', '.css':'text/css', '.xml':'text/xml', '.txt':'text/plain' };
const encoded = p => fs.readFileSync(p).toString('base64');
const resources = [];
for (const [name, meta] of redirects) {
  if (name === 'google-ima-dai.js' || meta.params?.length) continue;
  const kind = mime[path.extname(name)] ?? 'text/plain';
  resources.push({ name, aliases: typeof meta.alias === 'string' ? [meta.alias] : meta.alias ?? [], kind: { mime: kind }, content: encoded(path.join(uboRoot, 'src/web_accessible_resources', name)) });
}
for (const meta of JSON.parse(fs.readFileSync(path.join(braveRoot, 'metadata.json')))) {
  resources.push({ name: meta.name, aliases: meta.aliases, kind: meta.kind, content: encoded(path.join(braveRoot, 'resources', meta.resourcePath)), permission: 2 });
}
for (const s of builtinScriptlets) {
  resources.push({ name:s.name, aliases:s.aliases ?? [], kind:{ mime:'application/javascript' }, content:Buffer.from(s.fn.toString()).toString('base64'), dependencies:s.dependencies ?? [], permission:s.requiresTrust ? 1 : 0 });
}
const names = new Set(resources.map(r => r.name));
const aliases = new Set();
for (const r of resources) {
  for (const dep of r.dependencies ?? []) if (!names.has(dep)) throw Error(`Missing dependency: ${r.name} -> ${dep}`);
  for (const name of [r.name, ...r.aliases]) {
    if (aliases.has(name)) throw Error(`Duplicate resource/alias: ${name}`);
    aliases.add(name);
  }
}
fs.writeFileSync(output, JSON.stringify(resources));
console.log(JSON.stringify({ resources:resources.length, sha256:createHash('sha256').update(fs.readFileSync(output)).digest('hex') }));
