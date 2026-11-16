const canvas = document.getElementById("screen");
const ctx = canvas.getContext("2d", { alpha: false });

const MIME_TYPES = {
  1: "image/webp",
  2: "image/jpeg",
  3: "image/png",
  4: "image/avif",
};

const ws = new WebSocket(`wss://${location.host}/`);
ws.binaryType = "arraybuffer";

ws.onmessage = async (event) => {
  const view = new DataView(event.data);
  const codec = view.getUint8(0);
  const x = view.getUint16(1, true);
  const y = view.getUint16(3, true);
  const w = view.getUint16(5, true);
  const h = view.getUint16(7, true);

  // Auto-resize the canvas if a full-screen frame arrives with new dimensions
  if (x === 0 && y === 0 && (canvas.width !== w || canvas.height !== h)) {
    canvas.width = w;
    canvas.height = h;
  }

  const mime = MIME_TYPES[codec];
  if (!mime) return;

  const blob = new Blob([event.data.slice(9)], { type: mime });
  const bitmap = await createImageBitmap(blob);

  ctx.drawImage(bitmap, x, y);
  bitmap.close(); // Immediately free the GPU/decoded texture memory
};