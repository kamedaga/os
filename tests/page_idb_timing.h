/* Opt-in diagnostic only. Never write database contents or log keys/results.
 * Preserve native return objects/exceptions; at most 32 event pairs for 20 s.
 * The isolated-world baseline probe remains independent of this page-world
 * API wrapper. Do not use these runs as zero-overhead speed comparisons. */
static const char page_idb_script[] =
    "(()=>{'use strict';const key='__pachaIdbTiming';if(Object.hasOwn(globalThis,key))return;"
    "const now=performance.now.bind(performance),p={start:now(),active:true,events:[]},undo=[];"
    "Object.defineProperty(globalThis,key,{value:p,configurable:true});"
    "function stop(){p.active=false;for(const [o,n,d,w]of undo){if(o[n]===w)Object.defineProperty(o,n,d)}undo.length=0}"
    "function wrap(o,n,kind,ok,bad){const d=Object.getOwnPropertyDescriptor(o,n);if(!d||typeof d.value!=='function'||!d.configurable)return;"
    "const original=d.value;function wrapped(...args){if(!p.active||p.events.length>=32)return Reflect.apply(original,this,args);"
    "const start=now();if(start-p.start>=20000){stop();return Reflect.apply(original,this,args)}"
    "const request=Reflect.apply(original,this,args),row={op:kind,start,returned:now()};p.events.push(row);"
    "if(kind==='transaction'){row.mode=request.mode;if(p.events.length<=8){setTimeout(()=>{row.turn=now()},0);setTimeout(()=>{row.turn250=now()},250)}}"
    "const finish=e=>{row.end=now();row.event=e.type;request.removeEventListener(ok,finish);request.removeEventListener(bad,finish)};"
    "request.addEventListener(ok,finish);request.addEventListener(bad,finish);return request}"
    "Object.defineProperty(o,n,{...d,value:wrapped});undo.push([o,n,d,wrapped])}"
    "try{if(globalThis.IDBFactory)wrap(IDBFactory.prototype,'open','open','success','error');"
    "if(globalThis.IDBDatabase)wrap(IDBDatabase.prototype,'transaction','transaction','complete','abort');"
    "if(globalThis.IDBObjectStore)wrap(IDBObjectStore.prototype,'get','get','success','error')}catch(e){stop();p.unavailable=true}"
    "setTimeout(stop,20000)})()";
