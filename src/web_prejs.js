// Runs before the Emscripten module starts.
//
// The engine forces -sALLOW_MEMORY_GROWTH=1, so on recent Chrome the wasm
// memory buffer is a *resizable* ArrayBuffer. Several browser APIs now reject
// views backed by a resizable/growable buffer ("must not be resizable"):
//   - TextDecoder.decode  (emscripten UTF8ToString — crashes on first syscall)
//   - GPUQueue.writeBuffer / writeTexture  (every GPU upload)
// Emscripten 6.0.2 doesn't guard against this. We can't reliably flip the
// engine's growth flag from here (link order), so patch the offending APIs to
// copy any resizable-backed view into a plain buffer first.

(function () {
  function toFixed(data) {
    // Typed-array view backed by a resizable/growable buffer -> copy.
    if (data && data.buffer && (data.buffer.resizable || data.buffer.growable)) {
      return data.slice();
    }
    // A resizable ArrayBuffer passed directly.
    if (data instanceof ArrayBuffer && data.resizable) {
      return data.slice(0);
    }
    return data;
  }

  if (typeof TextDecoder !== 'undefined' && !TextDecoder.prototype.__resizableFix) {
    var dec = TextDecoder.prototype.decode;
    TextDecoder.prototype.decode = function (input, options) {
      return dec.call(this, toFixed(input), options);
    };
    TextDecoder.prototype.__resizableFix = true;
  }

  // Browser autoplay policy starts every AudioContext suspended until a user
  // gesture. We can't bypass that, but we can make it seamless: track every
  // AudioContext miniaudio creates and resume them on the *first* key/pointer
  // event. Since playing Doom needs input immediately, sound effectively starts
  // on its own — no click-to-start button. (This pre-js runs before miniaudio
  // creates its context, so the constructor wrap catches it.)
  (function () {
    var Native = window.AudioContext || window.webkitAudioContext;
    if (!Native || Native.__autoResume) return;
    var contexts = [];
    function Patched() {
      var c = new (Function.prototype.bind.apply(Native, [null].concat([].slice.call(arguments))))();
      contexts.push(c);
      return c;
    }
    Patched.prototype = Native.prototype;
    Patched.__autoResume = true;
    window.AudioContext = Patched;
    if (window.webkitAudioContext) window.webkitAudioContext = Patched;
    function resumeAll() {
      for (var i = 0; i < contexts.length; i++) {
        var c = contexts[i];
        if (c && c.state === 'suspended' && c.resume) c.resume();
      }
    }
    ['keydown', 'pointerdown', 'mousedown', 'touchstart', 'click'].forEach(function (t) {
      window.addEventListener(t, resumeAll, { capture: true, passive: true });
    });
  })();

  if (typeof GPUQueue !== 'undefined' && !GPUQueue.prototype.__resizableFix) {
    var wb = GPUQueue.prototype.writeBuffer;
    GPUQueue.prototype.writeBuffer = function (buffer, bufferOffset, data, dataOffset, size) {
      return wb.call(this, buffer, bufferOffset, toFixed(data), dataOffset, size);
    };
    var wt = GPUQueue.prototype.writeTexture;
    GPUQueue.prototype.writeTexture = function (destination, data, dataLayout, size) {
      return wt.call(this, destination, toFixed(data), dataLayout, size);
    };
    GPUQueue.prototype.__resizableFix = true;
  }
})();
