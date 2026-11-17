// Windowed-sinc polyphase interpolator. Linear interpolation has a frequency
// response that depends on the fractional read position (flat at frac=0, -6dB
// at 16kHz at frac=0.5). With a playback rate slightly off 1.0 the fraction
// sweeps continuously, which audibly modulates the treble ("talking through a
// fan"). A band-limited kernel has a near-constant response for every phase.
const SINC_HALF = 8;               // taps on each side of the read point
const SINC_TAPS = SINC_HALF * 2;
const SINC_PHASES = 256;
const SINC_TABLE = (() => {
    const table = new Float32Array((SINC_PHASES + 1) * SINC_TAPS);
    const cutoff = 0.92; // fraction of Nyquist; leaves headroom for rate > 1
    const besselI0 = (x) => {
        let sum = 1, term = 1;
        for (let k = 1; k < 30; k++) {
            term *= (x / (2 * k)) * (x / (2 * k));
            sum += term;
        }
        return sum;
    };
    const beta = 8.0;
    const i0Beta = besselI0(beta);
    for (let p = 0; p <= SINC_PHASES; p++) {
        const frac = p / SINC_PHASES;
        let sum = 0;
        for (let t = 0; t < SINC_TAPS; t++) {
            // Tap t corresponds to sample offset (t - SINC_HALF + 1) from readIdx
            const x = (t - SINC_HALF + 1) - frac;
            const sinc = x === 0 ? 1 : Math.sin(Math.PI * cutoff * x) / (Math.PI * cutoff * x);
            const r = x / SINC_HALF;
            const win = Math.abs(r) >= 1 ? 0 : besselI0(beta * Math.sqrt(1 - r * r)) / i0Beta;
            const v = sinc * win;
            table[p * SINC_TAPS + t] = v;
            sum += v;
        }
        // Normalize so every phase has exactly unity DC gain (no amplitude ripple)
        for (let t = 0; t < SINC_TAPS; t++) table[p * SINC_TAPS + t] /= sum;
    }
    return table;
})();

class PcmRingProcessor extends AudioWorkletProcessor {
    constructor() {
        super();
        // ~1.0 second ring buffer (interleaved stereo floats)
        this.capacityFrames = Math.floor(sampleRate * 1.0);
        this.buffer = new Float32Array(this.capacityFrames * 2);
        this.writeIdx = 0;
        this.readIdx = 0;
        this.readFrac = 0;
        this.availableFrames = 0;

        // Adaptive jitter buffer parameters (in ms; server sends 20ms / 960-frame packets)
        this.minTargetMs = 30;
        this.maxTargetMs = 250;
        this.targetMs = 40;
        this.stepUpMs = 15;
        this.stepDownMs = 5;
        this.excessMarginMs = 80;

        this.buffering = true;
        this.stableFrames = 0;
        this.stableWindowMs = 6000;
        this.framesSinceStepDown = Infinity;
        this.cleanStepDowns = 0;

        // Clock drift compensation (PI controller on smoothed buffer fill)
        this.avgFillFrames = this.msToFrames(this.targetMs);
        this.integralErrorSec = 0;
        this.kp = 0.08;
        this.ki = 0.0016;
        // Keep rate correction subtle: real clock drift is well under 0.1%,
        // and large offsets are audible as pitch wobble.
        this.maxRateOffset = 0.005;

        this.port.onmessage = (event) => {
            const incoming = new Float32Array(event.data);
            const incomingFrames = Math.floor(incoming.length / 2);
            for (let i = 0; i < incomingFrames; i++) {
                // Leave room so the interpolator's history behind readIdx isn't overwritten
                if (this.availableFrames < this.capacityFrames - SINC_TAPS) {
                    const dst = this.writeIdx * 2;
                    this.buffer[dst] = incoming[i * 2];
                    this.buffer[dst + 1] = incoming[i * 2 + 1];
                    this.writeIdx = (this.writeIdx + 1) % this.capacityFrames;
                    this.availableFrames++;
                }
            }

            // Trim oldest frames only if severe bufferbloat builds up (e.g. after a network stall)
            const maxFrames = Math.min(
                this.capacityFrames,
                this.msToFrames(this.targetMs + this.excessMarginMs)
            );
            if (this.availableFrames > maxFrames) {
                const keepFrames = this.msToFrames(this.targetMs + 20);
                const dropFrames = this.availableFrames - keepFrames;
                this.readIdx = (this.readIdx + dropFrames) % this.capacityFrames;
                this.readFrac = 0;
                this.availableFrames -= dropFrames;
                this.avgFillFrames = this.availableFrames;
            }
        };
    }

    msToFrames(ms) {
        return Math.round((ms / 1000) * sampleRate);
    }

    onUnderrun() {
        // If we dropped out shortly after lowering the buffer, back off the probe interval
        if (this.framesSinceStepDown < this.msToFrames(this.stableWindowMs)) {
            this.stableWindowMs = Math.min(32000, this.stableWindowMs * 2);
        }
        this.cleanStepDowns = 0;
        this.stableFrames = 0;
        this.targetMs = Math.min(this.maxTargetMs, this.targetMs + this.stepUpMs);
        this.buffering = true;
    }

    process(inputs, outputs) {
        const outLeft = outputs[0][0];
        const outRight = outputs[0][1];
        const outLen = outLeft.length;
        const targetFrames = Math.max(outLen + SINC_HALF + 2, this.msToFrames(this.targetMs));

        if (this.buffering) {
            if (this.availableFrames < targetFrames) {
                outLeft.fill(0);
                outRight.fill(0);
                return true;
            }
            this.buffering = false;
            this.stableFrames = 0;
            this.readFrac = 0;
            this.avgFillFrames = this.availableFrames;
        }

        // Smooth the buffer fill level (~1s EMA) to filter out 20ms packet sawtooth & jitter
        const dt = outLen / sampleRate;
        const alpha = dt / (1.0 + dt);
        this.avgFillFrames += alpha * (this.availableFrames - this.avgFillFrames);

        // PI controller adjusts playback rate slightly to match server audio clock
        const errorSec = (this.avgFillFrames - targetFrames) / sampleRate;
        this.integralErrorSec = Math.max(
            -5.0,
            Math.min(5.0, this.integralErrorSec + errorSec * dt)
        );
        const rateOffset = Math.max(
            -this.maxRateOffset,
            Math.min(this.maxRateOffset, this.kp * errorSec + this.ki * this.integralErrorSec)
        );
        const step = 1.0 + rateOffset;

        const maxFramesNeeded = Math.ceil(outLen * step) + SINC_HALF + 2;
        if (this.availableFrames < maxFramesNeeded) {
            this.onUnderrun();
            outLeft.fill(0);
            outRight.fill(0);
            return true;
        }

        const buf = this.buffer;
        const cap = this.capacityFrames;
        let readIdx = this.readIdx;
        let readFrac = this.readFrac;
        let available = this.availableFrames;

        const table = SINC_TABLE;
        for (let i = 0; i < outLen; i++) {
            // Blend the two nearest precomputed phases for a smooth kernel
            const phasePos = readFrac * SINC_PHASES;
            const phase = Math.min(SINC_PHASES - 1, Math.floor(phasePos));
            const pf = phasePos - phase;
            const k0 = phase * SINC_TAPS;
            const k1 = k0 + SINC_TAPS;

            let l = 0, r = 0;
            let idx = (readIdx - SINC_HALF + 1 + cap) % cap;
            for (let t = 0; t < SINC_TAPS; t++) {
                const c = table[k0 + t] + (table[k1 + t] - table[k0 + t]) * pf;
                const j = idx * 2;
                l += buf[j] * c;
                r += buf[j + 1] * c;
                idx = idx + 1 === cap ? 0 : idx + 1;
            }
            outLeft[i] = l;
            outRight[i] = r;

            readFrac += step;
            if (readFrac >= 1.0) {
                const adv = Math.floor(readFrac);
                readFrac -= adv;
                readIdx = (readIdx + adv) % cap;
                available -= adv;
            }
        }

        this.readIdx = readIdx;
        this.readFrac = readFrac;
        this.availableFrames = available;

        this.stableFrames += outLen;
        this.framesSinceStepDown += outLen;

        if (this.targetMs > this.minTargetMs &&
            this.stableFrames >= this.msToFrames(this.stableWindowMs)) {
            this.targetMs = Math.max(this.minTargetMs, this.targetMs - this.stepDownMs);
            this.stableFrames = 0;
            this.framesSinceStepDown = 0;
            this.cleanStepDowns++;
            if (this.cleanStepDowns >= 2) {
                this.stableWindowMs = Math.max(6000, this.stableWindowMs / 2);
                this.cleanStepDowns = 0;
            }
        }

        return true;
    }
}

registerProcessor("pcm-ring-processor", PcmRingProcessor);