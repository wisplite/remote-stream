const canvas = document.getElementById("screen");
const ctx = canvas.getContext("2d", { alpha: false });

// First byte of every message; must match MessageType in src/main.cpp
const MSG_SCREEN_INFO = 1;
const MSG_TILE = 2;
const MSG_CURSOR = 3;

const MIME_TYPES = {
  1: "image/webp",
  2: "image/jpeg",
  3: "image/png",
  4: "image/avif",
};

// Scale the canvas to fit the window while keeping the desktop's aspect ratio
function fitCanvas() {
  const scale = Math.min(window.innerWidth / canvas.width, window.innerHeight / canvas.height);
  canvas.style.width = `${Math.floor(canvas.width * scale)}px`;
  canvas.style.height = `${Math.floor(canvas.height * scale)}px`;
  applyCursor();
}
window.addEventListener("resize", fitCanvas);

// Host cursor, drawn by the browser as a CSS cursor so it tracks the local pointer
// with no round-trip latency. Re-rendered on resize to match the canvas scale.
let hostCursor = null; // { image: HTMLCanvasElement, xhot, yhot } | { hidden: true }
const MAX_CSS_CURSOR = 128; // Browsers ignore larger cursor images

function setHostCursor(view, buffer) {
  const w = view.getUint16(1, true);
  const h = view.getUint16(3, true);
  const xhot = view.getUint16(5, true);
  const yhot = view.getUint16(7, true);
  const pixels = new Uint8ClampedArray(buffer, 9, w * h * 4);

  let visible = false;
  for (let i = 3; i < pixels.length; i += 4) {
    if (pixels[i] !== 0) { visible = true; break; }
  }
  if (!visible || w === 0 || h === 0) {
    hostCursor = { hidden: true };
  } else {
    const image = document.createElement("canvas");
    image.width = w;
    image.height = h;
    image.getContext("2d").putImageData(new ImageData(pixels.slice(), w, h), 0, 0);
    hostCursor = { image, xhot, yhot };
  }
  applyCursor();
}

function applyCursor() {
  if (!hostCursor) {
    canvas.style.cursor = "";
    return;
  }
  if (hostCursor.hidden) {
    canvas.style.cursor = "none";
    return;
  }

  const { image, xhot, yhot } = hostCursor;
  const rectWidth = canvas.getBoundingClientRect().width;
  // Canvas is hidden until the first ScreenInfo arrives, so its rendered width
  // is 0; wait until it is visible rather than scaling the cursor down to 1x1.
  if (!rectWidth || !canvas.width) return;
  const displayScale = rectWidth / canvas.width;
  const scale = Math.min(displayScale, MAX_CSS_CURSOR / image.width, MAX_CSS_CURSOR / image.height);
  const w = Math.max(1, Math.round(image.width * scale));
  const h = Math.max(1, Math.round(image.height * scale));

  const scaled = document.createElement("canvas");
  scaled.width = w;
  scaled.height = h;
  const sctx = scaled.getContext("2d");
  sctx.imageSmoothingQuality = "high";
  sctx.drawImage(image, 0, 0, w, h);

  // The hotspot must lie inside the image or the browser rejects the cursor
  const hx = Math.min(w - 1, Math.round(xhot * scale));
  const hy = Math.min(h - 1, Math.round(yhot * scale));
  canvas.style.cursor = `url(${scaled.toDataURL("image/png")}) ${hx} ${hy}, auto`;
}

// Decodes run in parallel, but results are applied strictly in arrival order
let applyQueue = Promise.resolve();

const ws = new WebSocket(`wss://${location.host}/`);
ws.binaryType = "arraybuffer";

ws.onmessage = (event) => {
  const view = new DataView(event.data);
  const type = view.getUint8(0);

  if (type === MSG_SCREEN_INFO) {
    const w = view.getUint16(1, true);
    const h = view.getUint16(3, true);
    applyQueue = applyQueue.then(() => {
      const wasHidden = canvas.hidden;
      canvas.hidden = false;
      if (canvas.width !== w || canvas.height !== h) {
        canvas.width = w;
        canvas.height = h;
        fitCanvas();
      } else if (wasHidden) {
        fitCanvas();
      }
    });
  } else if (type === MSG_CURSOR) {
    setHostCursor(view, event.data);
  } else if (type === MSG_TILE) {
    const mime = MIME_TYPES[view.getUint8(1)];
    if (!mime) return;
    const x = view.getUint16(2, true);
    const y = view.getUint16(4, true);

    const decoded = createImageBitmap(new Blob([event.data.slice(10)], { type: mime }));
    applyQueue = applyQueue
      .then(() => decoded)
      .then((bitmap) => {
        ctx.drawImage(bitmap, x, y);
        bitmap.close(); // Immediately free the GPU/decoded texture memory
      })
      .catch((err) => console.error("Failed to draw tile", err));
  }
};

ws.onclose = () => {
  hostCursor = null;
  applyCursor();
};

// Input handling
function getRemoteCoords(event) {
    const rect = canvas.getBoundingClientRect();
    const scaleX = canvas.width / rect.width;
    const scaleY = canvas.height / rect.height;
    return {
        x: Math.round((event.clientX - rect.left) * scaleX),
        y: Math.round((event.clientY - rect.top) * scaleY),
    };
}

function sendKey(keysym, pressed) {
    // send key is 0x13, uint32 keysym, uint8 pressed
    const view = new DataView(new ArrayBuffer(8));
    view.setUint8(0, 0x13);
    view.setUint32(1, keysym, true);
    view.setUint8(5, pressed ? 1 : 0);
    ws.send(view.buffer);
}

function moveMouse(x, y, button, pressed) {
    // mouse move is 0x10, uint16 x, uint16 y
    const view = new DataView(new ArrayBuffer(6));
    view.setUint8(0, 0x10);
    view.setUint16(1, x, true);
    view.setUint16(3, y, true);
    ws.send(view.buffer);
}

function pressMouseButton(button, pressed) {
    // mouse button is 0x11, uint8 button, uint8 pressed
    const view = new DataView(new ArrayBuffer(4));
    view.setUint8(0, 0x11);
    view.setUint8(1, button + 1);
    view.setUint8(2, pressed ? 1 : 0);
    ws.send(view.buffer);
}

function scrollMouse(delta_x, delta_y) {
    // mouse scroll is 0x12, int8 delta_x, int8 delta_y
    const view = new DataView(new ArrayBuffer(4));
    view.setUint8(0, 0x12);
    view.setInt8(1, delta_x);
    view.setInt8(2, delta_y);
    ws.send(view.buffer);
}

canvas.tabIndex = 0;
canvas.addEventListener("contextmenu", (event) => event.preventDefault());
canvas.addEventListener("keydown", (event) => {
    event.preventDefault();
    const keysym = eventToKeysym(event);
    if (keysym) sendKey(keysym, true);
});
canvas.addEventListener("keyup", (event) => {
    event.preventDefault();
    const keysym = eventToKeysym(event);
    if (keysym) sendKey(keysym, false);
});
canvas.addEventListener("mousemove", (event) => {
    const { x, y } = getRemoteCoords(event);
    moveMouse(x, y);
});
canvas.addEventListener("mousedown", (event) => {
    const { button } = event;
    pressMouseButton(button, true);
});
canvas.addEventListener("mouseup", (event) => {
    const { button } = event;
    pressMouseButton(button, false);
});
canvas.addEventListener("wheel", (event) => {
    const { deltaX, deltaY } = event;
    scrollMouse(deltaX, deltaY);
});