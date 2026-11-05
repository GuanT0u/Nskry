import { z } from 'zod';
import semver from 'semver';
import { readFileSync, readdirSync } from 'node:fs';
import { resolve } from 'node:path';
const text = z.string().trim().min(1);
const https = text.refine(v => { try { const u = new URL(v); return u.protocol === 'https:' && !u.username && !u.password; } catch { return false; } }, 'Requires HTTPS without credentials');
const version = text.refine(v => semver.valid(v) === v, 'Requires canonical SemVer');
const date = z.string().regex(/^\d{4}-\d{2}-\d{2}$/).refine(v => { const d = new Date(v); return !isNaN(+d) && d.toISOString().startsWith(v); });
const asset = z.object({ name: text, url: https, bytes: z.number().int().positive().safe(), sha256: z.string().regex(/^[a-fA-F0-9]{64}$/), includes: z.array(text).min(1), setup: text }).strict();
const common = { version, date, status: z.enum(['development','prerelease','stable','withdrawn']), source: https, notes: text, assets: z.array(asset), min_nskry: version, max_nskry: version.nullable() };
export const pluginSchema = z.object({ ...common, id: z.string().regex(/^[a-z0-9][a-z0-9.-]*$/), name: text, summary: text, publisher: text, origin: z.enum(['official','third-party']), license: text, support: https, tags: z.array(text).min(1), api: z.literal(2), privacy: text, limitations: text, catalog: z.boolean(), update_url: https.nullable() }).strict();
export const releaseSchema = z.object({ ...common, id: version }).strict();
const names = { development:'开发中', prerelease:'预发布', stable:'稳定版', withdrawn:'已撤回' };
export const statusLabel = value => names[value];
export function validateRecords(plugins, releases) {
  const unique = (items, label) => { const ids = new Set(); for (const item of items) { if (ids.has(item.id)) throw new Error(`Duplicate ${label}: ${item.id}`); ids.add(item.id); } };
  plugins = plugins.map(p => pluginSchema.parse(p)); releases = releases.map(r => releaseSchema.parse(r));
  unique(plugins, 'plugin'); unique(releases, 'release');
  for (const item of [...plugins, ...releases]) {
    if (item.max_nskry && semver.gt(item.min_nskry, item.max_nskry)) throw new Error('Reversed compatibility range');
    if (['stable','prerelease'].includes(item.status)) {
      if (!item.assets.length) throw new Error('Published release requires assets');
      if (/\/(master|main|latest)(\/|$)/.test(new URL(item.source).pathname)) throw new Error('Published source must reference a fixed version');
      for (const a of item.assets) if (/\/(latest|master|main)(\/|$)/.test(new URL(a.url).pathname)) throw new Error('Published assets must reference a fixed version');
    }
    const urls = new Set();
    for (const a of item.assets) { if (urls.has(a.url)) throw new Error('Duplicate asset'); urls.add(a.url); }
  }
  for (const r of releases) if (r.id !== r.version) throw new Error('Release ID/version mismatch');
  for (const p of plugins) {
    if (p.catalog && (p.origin !== 'official' || p.status !== 'stable' || p.assets.length !== 1)) throw new Error('Catalog requires one stable official package, without ambiguous variants');
    for (const a of p.assets) if (!new URL(a.url).pathname.endsWith('.nskryplugin')) throw new Error('Plugin asset must be .nskryplugin');
  }
  return { plugins, releases };
}
export function loadData() {
  const read = folder => readdirSync(resolve('src/data', folder)).filter(n => n.endsWith('.json')).sort().map(n => JSON.parse(readFileSync(resolve('src/data', folder, n), 'utf8')));
  return validateRecords(read('plugins'), read('releases'));
}
export function makeCatalog(plugins) {
  const validated = validateRecords(plugins, []).plugins;
  return { schema:1, plugins: validated.filter(p => p.catalog).map(p => ({ id:p.id, latest_version:p.version, package_url:p.assets[0].url, sha256:p.assets[0].sha256, min_nskry:p.min_nskry, max_nskry:p.max_nskry, release_notes:p.notes })) };
}
export const repo = 'https://github.com/GuanT0u/Nskry';
