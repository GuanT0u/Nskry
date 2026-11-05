import {mkdirSync,writeFileSync} from 'node:fs';
import {loadData,makeCatalog} from '../src/lib/data.mjs';
const output=new URL('../.astro/catalog-preview/',import.meta.url);
mkdirSync(output,{recursive:true});
writeFileSync(new URL('plugins-v1.json',output),JSON.stringify(makeCatalog(loadData().plugins),null,2)+'\n');
console.log('Preview only: .astro/catalog-preview/plugins-v1.json. Not copied to dist or configured in Nskry.');
