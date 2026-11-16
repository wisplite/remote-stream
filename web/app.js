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
