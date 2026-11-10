// Owns browser input only. Camera integration and rendering stay in Viewport.
// An asynchronous pointer-lock grant may arrive after the user releases RMB.
export class FlyControls {
  constructor(surface, environment, onStop) {
    this.surface = surface;
    this.environment = environment;
    this.onStop = onStop;
    this.active = false;
    this.keys = new Set();

    environment.window.addEventListener('blur', () => this.stop());
    environment.document.addEventListener('pointerlockchange', () => {
      const locked = environment.document.pointerLockElement === surface;
      if (locked && !this.active) environment.document.exitPointerLock();
      if (!locked) this.stop();
    });
    environment.window.addEventListener('keydown', event => {
      if (!this.active) return;
      if (event.code === 'Escape') {
        this.stop();
        return;
      }
      if (['KeyW', 'KeyA', 'KeyS', 'KeyD', 'KeyQ', 'KeyE', 'ShiftLeft', 'ShiftRight'].includes(event.code)) {
        event.preventDefault();
        this.keys.add(event.code);
      }
    });
    environment.window.addEventListener('keyup', event => this.keys.delete(event.code));
  }

  start(pointerId) {
    this.active = true;
    this.surface.focus({preventScroll: true});
    this.surface.setPointerCapture(pointerId);
    // Captured dragging remains available if pointer lock is unsupported/denied.
    // Older implementations return void and can throw instead of rejecting.
    try {
      this.surface.requestPointerLock?.()?.catch(() => {});
    } catch {
      // No permission prompt or retry is needed for the fallback.
    }
  }

  stop() {
    this.active = false;
    this.keys.clear();
    if (this.environment.document.pointerLockElement === this.surface) {
      this.environment.document.exitPointerLock();
    }
    this.onStop();
  }

  lostCapture() {
    // Entering pointer lock can transfer capture; keep flying in that case.
    if (this.environment.document.pointerLockElement !== this.surface) this.stop();
  }
}
