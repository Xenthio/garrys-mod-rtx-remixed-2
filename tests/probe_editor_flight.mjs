import assert from 'node:assert/strict';
import {FlyControls} from '../tools/probe_editor/fly_controls.mjs';

function fixture(request = () => {}) {
  const handlers = {};
  const listen = (name, callback) => { handlers[name] = callback; };
  let exited = 0, stopped = 0, captured, focused = false;
  const surface = {
    focus: () => { focused = true; },
    setPointerCapture: id => { captured = id; },
    requestPointerLock: request
  };
  const document = {
    pointerLockElement: null,
    addEventListener: listen,
    exitPointerLock: () => { exited++; document.pointerLockElement = null; }
  };
  const controls = new FlyControls(surface, {
    window: {addEventListener: listen}, document
  }, () => stopped++);
  return {controls, surface, document, handlers,
    result: () => ({exited, stopped, captured, focused})};
}

// A late grant must immediately relinquish the cursor without restarting flight.
const late = fixture();
late.controls.start(9);
assert.equal(late.result().focused, true);
assert.equal(late.result().captured, 9);
late.controls.stop();
late.document.pointerLockElement = late.surface;
late.handlers.pointerlockchange();
assert.equal(late.result().exited, 1);
assert.equal(late.controls.active, false);

// Capture transfer into pointer lock keeps flight active; Escape releases it.
const locked = fixture();
locked.controls.start(1);
locked.document.pointerLockElement = locked.surface;
locked.handlers.pointerlockchange();
locked.controls.lostCapture();
assert.equal(locked.controls.active, true);
let prevented = 0;
locked.handlers.keydown({code: 'KeyW', preventDefault: () => prevented++});
assert.equal(prevented, 1);
assert.equal(locked.controls.keys.has('KeyW'), true);
locked.handlers.keyup({code: 'KeyW'});
assert.equal(locked.controls.keys.size, 0);
locked.handlers.keydown({code: 'Escape'});
assert.equal(locked.controls.active, false);
assert.equal(locked.result().exited, 1);

// No keyboard interception while editing properties; focus loss stops movement.
const fallback = fixture(() => { throw Error('Pointer lock declined'); });
fallback.handlers.keydown({code: 'KeyW', preventDefault: () => prevented++});
assert.equal(prevented, 1);
fallback.controls.start(2);
assert.equal(fallback.controls.active, true);
fallback.handlers.keydown({code: 'KeyD', preventDefault: () => prevented++});
fallback.handlers.blur();
assert.equal(fallback.controls.active, false);
assert.equal(fallback.controls.keys.size, 0);
fallback.controls.start(3);
fallback.controls.lostCapture();
assert.equal(fallback.controls.active, false);

// Promise rejection leaves captured flight available without unhandled errors.
const declined = fixture(() => Promise.reject(Error('Denied')));
declined.controls.start(4);
await new Promise(resolve => setTimeout(resolve, 0));
assert.equal(declined.controls.active, true);
declined.controls.stop();
console.log('Fly controls: late lock, capture transfer, Escape, blur, property focus and denied-lock checks passed');
