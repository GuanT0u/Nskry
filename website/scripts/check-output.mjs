import { readdirSync, readFileSync, existsSync, statSync } from 'node:fs';
import { resolve, relative, join, sep } from 'node:path';
const root=resolve('dist');
const base=('/'+(process.env.WEBSITE_BASE||'/').replace(/^\/+|\/+$/g,'')+'/').replace(/\/+/g,'/');
function files(dir){return readdirSync(dir,{withFileTypes:true}).flatMap(e=>e.isDirectory()?files(join(dir,e.name)):[join(dir,e.name)]);}
const output=files(root);let count=0;
for(const file of output.filter(p=>p.endsWith('.html'))){
 const html=readFileSync(file,'utf8');
 if(!html.includes('lang="zh-CN"')||!html.includes('name="viewport"'))throw new Error(`Missing document metadata: ${file}`);
 for(const [,raw] of html.matchAll(/(?:href|src)="([^"]+)"/g)){
  if(/^(https?:|mailto:|data:|#)/.test(raw))continue;
  const path=decodeURIComponent(raw.split(/[?#]/)[0]);
  if(!path)continue;
  let target;
  if(path.startsWith('/')){if(!path.startsWith(base))throw new Error(`Escapes configured base: ${raw}`);target=resolve(root,path.slice(base.length));}
  else target=resolve(file,'..',path);
  if(relative(root,target).startsWith('..'+sep))throw new Error(`Escapes output: ${raw}`);
  if(existsSync(target)&&statSync(target).isDirectory())target=join(target,'index.html');
  if(!existsSync(target))throw new Error(`Broken local reference in ${relative(root,file)}: ${raw}`);
  count++;
 }
}
if(existsSync(join(root,'catalog','plugins-v1.json')))throw new Error('Unapproved official catalog must not be publicly emitted');
console.log(`Output verified: ${output.filter(p=>p.endsWith('.html')).length} HTML pages, ${count} local references; ${output.reduce((s,p)=>s+statSync(p).size,0)} bytes total.`);
