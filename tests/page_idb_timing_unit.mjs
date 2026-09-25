import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

// Run the exact C-embedded script, not a separately maintained JS copy.
const header = readFileSync(new URL('./page_idb_timing.h', import.meta.url), 'utf8');
const script = header.split('\n').filter(l => /^\s*"/.test(l))
  .map(l => JSON.parse(l.trim().replace(/;$/, ''))).join('');
function setup() {
  let clock = 10, timer;
  const turns = [];
  class Factory {
    open(...args) {
      assert.equal(this, factory);
      if (args[0] === 'throw') throw sentinel;
      lastArgs = args;
      return lastRequest = new EventTarget();
    }
  }
  class Database {
    transaction(...args) {
      assert.equal(this, database);
      lastArgs = args;
      lastRequest = new EventTarget();
      lastRequest.mode = args[1] ?? 'readonly';
      return lastRequest;
    }
  }
  class Store {
    get(...args) {
      assert.equal(this, store);
      lastArgs = args;
      return lastRequest = new EventTarget();
    }
  }
  const factory = new Factory(), database = new Database(), store = new Store(), sentinel = {};
  let lastArgs, lastRequest;
  const open = Factory.prototype.open, transaction = Database.prototype.transaction;
  const get = Store.prototype.get;
  const context = vm.createContext({IDBFactory: Factory, IDBDatabase: Database, IDBObjectStore: Store,
    performance: {now: () => clock}, setTimeout: (fn, delay) => {
      if (delay === 0 || delay === 250) { turns.push({fn, delay}); return; }
      assert.equal(delay, 20000); timer = fn;
    }});
  vm.runInContext(script, context);
  return {context, factory, database, store, sentinel, open, transaction, get,
    get args() { return lastArgs; }, get request() { return lastRequest; },
    set time(value) { clock = value; }, stop() { timer(); },
    flushTurns(delay = 0) {
      assert(turns.length <= 16);
      for (let i = turns.length - 1; i >= 0; --i) {
        if (turns[i].delay === delay) turns.splice(i, 1)[0].fn();
      }
    }};
}

{
  const s = setup(), key = {}, request = s.factory.open(key, 7);
  assert.equal(request, s.request);
  assert.deepEqual(s.args, [key, 7]);
  s.time = 40;
  request.dispatchEvent(new Event('success'));
  const p = s.context.__pachaIdbTiming;
  assert.equal(p.events[0].op, 'open');
  assert.equal(p.events[0].start, 10);
  assert.equal(p.events[0].end, 40);
  assert.equal(p.events[0].event, 'success');
  s.time = 50;
  request.dispatchEvent(new Event('error'));
  assert.equal(p.events[0].end, 40); // listener removed after completion
  const tx = s.database.transaction(['records'], 'readonly');
  assert.equal(tx, s.request);
  tx.dispatchEvent(new Event('error')); // may be handled; not final abort
  assert.equal(p.events[1].end, undefined);
  tx.dispatchEvent(new Event('abort'));
  assert.equal(p.events[1].event, 'abort');
  assert.equal(p.events[1].mode, 'readonly');
  s.flushTurns();
  assert.equal(p.events[1].turn, 50);
  s.time = 300;
  s.flushTurns(250);
  assert.equal(p.events[1].turn250, 300);
  assert.throws(() => s.factory.open('throw'), e => e === s.sentinel);
  assert.equal(p.events.length, 2);
  const getRequest = s.store.get(key);
  assert.equal(getRequest, s.request);
  assert.equal(s.args[0], key);
  getRequest.dispatchEvent(new Event('success'));
  assert.equal(p.events[2].op, 'get');
  assert.equal(p.events[2].event, 'success');
  assert(!JSON.stringify(p).includes('records')); // no store names or data
  for (let i = 0; i < 1000; ++i) s.database.transaction('records');
  assert.equal(p.events.length, 32);
  s.stop();
  assert.equal(s.factory.open, s.open);
  assert.equal(s.database.transaction, s.transaction);
  assert.equal(s.store.get, s.get);
  assert.equal(p.active, false);
}
{
  const s = setup();
  s.time = 20010; // check deadline even if the timer has not run
  s.factory.open('db');
  assert.equal(s.context.__pachaIdbTiming.events.length, 0);
  assert.equal(s.factory.open, s.open);
}
{
  const s = setup(), replacement = function () {};
  Object.getPrototypeOf(s.factory).open = replacement;
  s.stop(); // never overwrite a later replacement installed by the page
  assert.equal(s.factory.open, replacement);
  assert.equal(s.database.transaction, s.transaction);
}
{
  const s = setup(), installed = s.factory.open;
  vm.runInContext(script, s.context);
  assert.equal(s.factory.open, installed); // no double wrapping
}
console.log('IndexedDB timing: identity, exceptions, bounded events, deadline, restore PASS');
