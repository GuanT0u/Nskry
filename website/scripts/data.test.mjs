import test from 'node:test';
import assert from 'node:assert/strict';
import {loadData,validateRecords,makeCatalog} from '../src/lib/data.mjs';
const data=loadData();
const base=()=>structuredClone(data.plugins[0]);
const asset={name:'complete.nskryplugin',url:'https://downloads.example.org/v1/complete.nskryplugin',bytes:123,sha256:'a'.repeat(64),includes:['runtime'],setup:'No external model required'};
const published=()=>({...base(),version:'1.0.0',status:'stable',source:'https://github.com/GuanT0u/Nskry/tree/v1.0.0',assets:[{...asset}],catalog:true});
test('current development records have no fake downloads or public updates',()=>{for(const p of data.plugins.filter(p=>p.status==='development')){assert.equal(p.assets.length,0);assert.equal(p.catalog,false);}assert.deepEqual(makeCatalog(data.plugins.filter(p=>!p.catalog)),{schema:1,plugins:[]});});
test('reject duplicate plugin IDs',()=>assert.throws(()=>validateRecords([base(),base()],[]),/Duplicate/));
test('reject invalid SemVer and dates',()=>{for(const patch of [{version:'1.0'},{version:'01.0.0'},{date:'2026-02-30'}])assert.throws(()=>validateRecords([{...base(),...patch}],[]));});
test('reject published entries without assets',()=>assert.throws(()=>validateRecords([{...base(),status:'stable'}],[]),/assets/));
test('reject insecure URLs, missing hashes and wrong package suffix',()=>{for(const a of [{...asset,url:'http://example.org/a.nskryplugin'},{...asset,sha256:'bad'},{...asset,url:'https://user:secret@example.org/a.nskryplugin'},{...asset,url:'https://example.org/a.zip'}])assert.throws(()=>validateRecords([{...published(),assets:[a]}],[]));});
test('reject incompatible range and invalid sizes',()=>{assert.throws(()=>validateRecords([{...base(),min_nskry:'2.0.0',max_nskry:'1.0.0'}],[]),/range/);assert.throws(()=>validateRecords([{...published(),assets:[{...asset,bytes:-1}]}],[]));});
test('catalog never mixes third-party, withdrawn or ambiguous variants',()=>{for(const patch of [{origin:'third-party'},{status:'withdrawn'},{assets:[asset,{...asset,name:'online.nskryplugin',url:'https://downloads.example.org/v1/online.nskryplugin'}]}])assert.throws(()=>makeCatalog([{...published(),...patch}]));});
test('catalog emits only the current client wire format',()=>{assert.deepEqual(makeCatalog([published()]),{schema:1,plugins:[{id:base().id,latest_version:'1.0.0',package_url:asset.url,sha256:asset.sha256,min_nskry:base().min_nskry,max_nskry:null,release_notes:base().notes}]});});
test('reject release route/version mismatch',()=>assert.throws(()=>validateRecords([],[{id:'1.0.0',version:'1.0.1',date:'2026-10-28',status:'stable',source:'https://example.org/source',notes:'Release',assets:[asset],min_nskry:'0.5.0',max_nskry:null}]),/mismatch/));

test('reject mutable publication URLs',()=>{assert.throws(()=>validateRecords([{...published(),source:'https://github.com/GuanT0u/Nskry/tree/master'}],[]),/fixed version/);assert.throws(()=>validateRecords([{...published(),assets:[{...asset,url:'https://example.org/latest/a.nskryplugin'}]}],[]),/fixed version/);});
