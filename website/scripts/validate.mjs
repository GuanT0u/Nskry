import { loadData, makeCatalog } from '../src/lib/data.mjs';
import { readdirSync, readFileSync, existsSync } from 'node:fs';
const { plugins, releases } = loadData();
for (const [items,folder] of [[plugins,'plugins'],[releases,'releases']]) {
  for(const item of items) if(!existsSync(new URL(`../src/content/${folder}/${item.id}.md`,import.meta.url))) throw new Error(`Missing article: ${item.id}`);
  for(const name of readdirSync(new URL(`../src/content/${folder}/`,import.meta.url)).filter(n=>n.endsWith('.md'))) if(!items.some(i=>i.id===name.slice(0,-3))) throw new Error(`Orphan article: ${name}`);
}
// First-party development records must agree with the actual source manifests.
for(const p of plugins.filter(p=>p.origin==='official' && p.status==='development')) {
  const path=new URL(`../../plugins/${p.id}/manifest.json`,import.meta.url);
  if(existsSync(path)) {
    const m=JSON.parse(readFileSync(path,'utf8'));
    if(m.version!==p.version || m.compatibility.nskry_min!==p.min_nskry || m.runtime.api_version!==p.api) throw new Error(`Stale manifest data: ${p.id}`);
  }
}
makeCatalog(plugins);
console.log(`Validated ${plugins.length} plugins and ${releases.length} releases. Official catalog is preview-only; not published.`);
