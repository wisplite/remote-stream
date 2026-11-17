const canvas = document.getElementById("screen");
const ctx = canvas.getContext("2d", { alpha: false });

// First byte of every message; must match MessageType in src/main.cpp
const MSG_SCREEN_INFO = 1;
const MSG_TILE = 2;
const MSG_CURSOR = 3;
const MSG_AUDIO = 4;
const MSG_H264_VIDEO = 5;
const MSG_PING = 6;
const MSG_STREAM_MODE = 7;

// Client -> server message types; must match ClientMessage in src/main.cpp
const CMSG_PONG = 0x20;
const CMSG_KEYFRAME_REQUEST = 0x21;
const CMSG_H264_UNSUPPORTED = 0x22;

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

const overlay = document.getElementById("overlay");
const overlayTitle = document.getElementById("overlay-title");
const overlaySubtitle = document.getElementById("overlay-subtitle");
const reconnectBtn = document.getElementById("reconnect-btn");

let ws = null;
let reconnectTimer = null;
let countdownTimer = null;
let reconnectAttempts = 0;
const BASE_RECONNECT_MS = 1000;
const MAX_RECONNECT_MS = 15000;

function clearReconnectTimers() {
  if (reconnectTimer !== null) {
    clearTimeout(reconnectTimer);
    reconnectTimer = null;
  }
  if (countdownTimer !== null) {
    clearInterval(countdownTimer);
    countdownTimer = null;
  }
}

function showOverlay(title, subtitle = "", showRetry = false) {
  overlayTitle.textContent = title;
  overlaySubtitle.textContent = subtitle;
  reconnectBtn.hidden = !showRetry;
  overlay.hidden = false;
}

function hideOverlay() {
  overlay.hidden = true;
}

function scheduleReconnect() {
  clearReconnectTimers();
  const delayMs = Math.min(BASE_RECONNECT_MS * Math.pow(2, reconnectAttempts), MAX_RECONNECT_MS);
  reconnectAttempts++;

  let remainingSec = Math.ceil(delayMs / 1000);
  const updateSubtitle = () => {
    overlaySubtitle.textContent = `Reconnecting in ${remainingSec}s...`;
  };

  showOverlay("Connection lost", "", true);
  updateSubtitle();

  countdownTimer = setInterval(() => {
    remainingSec = Math.max(0, remainingSec - 1);
    if (remainingSec > 0) {
      updateSubtitle();
    } else {
      clearInterval(countdownTimer);
      countdownTimer = null;
    }
  }, 1000);

  reconnectTimer = setTimeout(() => {
    reconnectTimer = null;
    connect();
  }, delayMs);
}

// H.264 tier. Every connection starts on tiles; the server switches a viewer to
// H.264 with a StreamMode message once its link proves stable, and back again
// (followed by a full tile frame) when the link degrades.
const STREAM_TILES = 0;
const STREAM_H264 = 1;
// Matches what x264 emits with the ultrafast preset (Constrained Baseline, level 5.2)
const H264_CODEC = "avc1.42e034";
const KEYFRAME_REQUEST_INTERVAL_MS = 500;

let streamMode = STREAM_TILES;
let videoDecoder = null;
let videoGeneration = 0; // bumped on every decoder reset so stale frames are dropped
let waitingForKeyframe = true;
let lastKeyframeRequestMs = -Infinity;
let screenWidth = 0;
let screenHeight = 0;

// Resolves once per page; the server keeps us on tiles if this is false
const h264Supported = (async () => {
  if (typeof VideoDecoder === "undefined") return false;
  try {
    const { supported } = await VideoDecoder.isConfigSupported({ codec: H264_CODEC, optimizeForLatency: true });
    return !!supported;
  } catch {
    return false;
  }
})();

function requestKeyframe() {
  const now = performance.now();
  if (now - lastKeyframeRequestMs < KEYFRAME_REQUEST_INTERVAL_MS) return;
  lastKeyframeRequestMs = now;
  sendMessage(new Uint8Array([CMSG_KEYFRAME_REQUEST]).buffer);
}

function closeH264Decoder() {
  videoGeneration++;
  if (videoDecoder && videoDecoder.state !== "closed") videoDecoder.close();
  videoDecoder = null;
  waitingForKeyframe = true;
}

function initH264Decoder() {
  closeH264Decoder();
  const generation = videoGeneration;
  videoDecoder = new VideoDecoder({
    output: (frame) => {
      // Chained so a frame can't be overdrawn by tiles that arrived before the switch,
      // and dropped if the decoder was reset (e.g. we fell back to tiles) meanwhile
      applyQueue = applyQueue.then(() => {
        if (generation === videoGeneration) ctx.drawImage(frame, 0, 0);
        frame.close();
      });
    },
    error: (error) => {
      console.error("Video decoder error", error);
      if (generation !== videoGeneration) return;
      // The decoder is closed now; the next chunk recreates it and waits for an IDR
      closeH264Decoder();
      requestKeyframe();
    },
  });
  videoDecoder.configure({
    codec: H264_CODEC,
    optimizeForLatency: true,
  });
}

function setStreamMode(mode) {
  if (mode === streamMode) return;
  streamMode = mode;
  if (mode === STREAM_H264) {
    initH264Decoder();
  } else {
    closeH264Decoder();
  }
}

function decodeH264(view, buffer) {
  if (streamMode !== STREAM_H264) return;
  const isKeyframe = view.getUint8(1) !== 0;
  if (!videoDecoder) initH264Decoder();

  if (waitingForKeyframe) {
    if (!isKeyframe) {
      requestKeyframe();
      return;
    }
    waitingForKeyframe = false;
  }
  // Skipping a delta frame would corrupt every frame after it, so resync from an IDR
  if (!isKeyframe && videoDecoder.decodeQueueSize > 3) {
    waitingForKeyframe = true;
    requestKeyframe();
    return;
  }

  videoDecoder.decode(new EncodedVideoChunk({
    type: isKeyframe ? "key" : "delta",
    timestamp: Number(view.getBigUint64(2, true)),
    data: new Uint8Array(buffer, 10),
  }));
}

function connect() {
  clearReconnectTimers();
  if (ws) {
    const prev = ws;
    ws = null;
    prev.close();
  }

  // Every connection starts on tiles
  streamMode = STREAM_TILES;
  closeH264Decoder();

  showOverlay("Connecting...", reconnectAttempts > 0 ? "Attempting to reconnect..." : "");
  const socket = new WebSocket(`wss://${location.host}/`);
  socket.binaryType = "arraybuffer";
  ws = socket;

  socket.onopen = () => {
    if (socket !== ws) return;
    reconnectAttempts = 0;
    clearReconnectTimers();
    hideOverlay();
    h264Supported.then((supported) => {
      if (!supported && socket === ws) socket.send(new Uint8Array([CMSG_H264_UNSUPPORTED]).buffer);
    });
  };

  socket.onmessage = (event) => {
    if (socket !== ws) return;
    const view = new DataView(event.data);
    const type = view.getUint8(0);

    if (type === MSG_PING) {
      // Echo immediately; the server derives RTT jitter from these to pick our tier
      const pong = new Uint8Array(5);
      pong[0] = CMSG_PONG;
      pong.set(new Uint8Array(event.data, 1, 4), 1);
      sendMessage(pong.buffer);
    } else if (type === MSG_STREAM_MODE) {
      setStreamMode(view.getUint8(1));
      if (streamMode === STREAM_H264) {
        console.log("Switching to H.264");
      } else {
        console.log("Switching to tiles");
      }
    } else if (type === MSG_SCREEN_INFO) {
      const w = view.getUint16(1, true);
      const h = view.getUint16(3, true);
      if ((w !== screenWidth || h !== screenHeight) && streamMode === STREAM_H264) {
        // The server restarts its encoder on resize; start clean from that IDR
        initH264Decoder();
      }
      screenWidth = w;
      screenHeight = h;
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
    } else if (type === MSG_AUDIO) {
      if (!audioNode || !audioDecoder || audioDecoder.state !== "configured") return;
      // Header: u8 type (1) + u8 channels (1) + u32 sample_rate (4) = 6 bytes
      const opusPayload = event.data.slice(6);
      audioDecoder.decode(new EncodedAudioChunk({
        type: "key",
        timestamp: audioTimestampUs,
        data: opusPayload,
      }));
      audioTimestampUs += 20000;
    } else if (type === MSG_H264_VIDEO) {
      decodeH264(view, event.data);
    }
  };

  socket.onclose = () => {
    if (socket !== ws) return;
    ws = null;
    hostCursor = null;
    streamMode = STREAM_TILES;
    closeH264Decoder();
    applyCursor();
    scheduleReconnect();
  };
}

reconnectBtn.addEventListener("click", () => {
  reconnectAttempts = 0;
  connect();
});

window.addEventListener("online", () => {
  if (!ws || ws.readyState === WebSocket.CLOSED) {
    reconnectAttempts = 0;
    connect();
  }
});

connect();

function sendMessage(buffer) {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(buffer);
  }
}

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
    sendMessage(view.buffer);
}

function moveMouse(x, y, button, pressed) {
    // mouse move is 0x10, uint16 x, uint16 y
    const view = new DataView(new ArrayBuffer(6));
    view.setUint8(0, 0x10);
    view.setUint16(1, x, true);
    view.setUint16(3, y, true);
    sendMessage(view.buffer);
}

function pressMouseButton(button, pressed) {
    // mouse button is 0x11, uint8 button, uint8 pressed
    const view = new DataView(new ArrayBuffer(4));
    view.setUint8(0, 0x11);
    view.setUint8(1, button + 1);
    view.setUint8(2, pressed ? 1 : 0);
    sendMessage(view.buffer);
}

function scrollMouse(delta_x, delta_y) {
    // mouse scroll is 0x12, int8 delta_x, int8 delta_y
    const view = new DataView(new ArrayBuffer(4));
    view.setUint8(0, 0x12);
    view.setInt8(1, delta_x);
    view.setInt8(2, delta_y);
    sendMessage(view.buffer);
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

// Audio handling
let audioNode = null;
let audioDecoder = null;
let audioTimestampUs = 0;

function initOpusDecoder() {
  if (audioDecoder) return;
  audioDecoder = new AudioDecoder({
    output: (audioData) => {
      const frames = audioData.numberOfFrames;
      const left = new Float32Array(frames);
      const right = new Float32Array(frames);

      audioData.copyTo(left, { planeIndex: 0, format: "f32-planar" });
      audioData.copyTo(right, { planeIndex: 1, format: "f32-planar" });
      audioData.close();

      const interleaved = new Float32Array(frames * 2);
      for (let i = 0; i < frames; i++) {
        interleaved[i * 2] = left[i];
        interleaved[i * 2 + 1] = right[i];
      }

      if (audioNode) {
        audioNode.port.postMessage(interleaved.buffer, [interleaved.buffer]);
      }
    },
    error: (error) => {
      console.error("Audio decoder error", error);
    },
  });

  audioDecoder.configure({
    codec: "opus",
    sampleRate: 48000,
    numberOfChannels: 2,
  });
}

async function initAudio() {
  if (audioNode) return;
  const audioCtx = new AudioContext({ sampleRate: 48000 });
  if (audioCtx.state === "suspended") {
    await audioCtx.resume();
  }
  await audioCtx.audioWorklet.addModule("audio-worklet.js");
  audioNode = new AudioWorkletNode(audioCtx, "pcm-ring-processor", {
    outputChannelCount: [2],
  });
  audioNode.connect(audioCtx.destination);
}

canvas.addEventListener("mousedown", () => {
  initAudio().then(() => initOpusDecoder());
}, { once: true });
