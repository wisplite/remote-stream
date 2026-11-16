const canvas = document.getElementById("screen");
const ctx = canvas.getContext("2d", { alpha: false });

// First byte of every message; must match MessageType in src/main.cpp
const MSG_SCREEN_INFO = 1;
const MSG_TILE = 2;

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
}
window.addEventListener("resize", fitCanvas);

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
      if (canvas.width !== w || canvas.height !== h) {
        canvas.width = w;
        canvas.height = h;
        fitCanvas();
      }
      canvas.hidden = false;
    });
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