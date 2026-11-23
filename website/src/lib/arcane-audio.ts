/**
 * Arcane Audio Synthesizer (Web Audio API)
 * Zero external audio assets required.
 * Generates tactile, mystical soundscapes procedurally:
 * - Water Droplets (for Scrying Well)
 * - Arcane Crystal Chimes (for Spell Gems)
 * - Tarnished Brass Clicks (for Lens & Sockets)
 * - Parchment Whispers (for Grimoire navigation)
 */

class ArcaneAudioEngine {
  private ctx: AudioContext | null = null;
  private isMuted: boolean = true;

  constructor() {
    if (typeof window !== 'undefined') {
      const saved = localStorage.getItem('nscry_sound_enabled');
      this.isMuted = saved !== 'true';
    }
  }

  private initContext() {
    if (!this.ctx && typeof window !== 'undefined') {
      const AudioCtx = window.AudioContext || (window as unknown as { webkitAudioContext: typeof AudioContext }).webkitAudioContext;
      if (AudioCtx) {
        this.ctx = new AudioCtx();
      }
    }
    if (this.ctx && this.ctx.state === 'suspended') {
      this.ctx.resume();
    }
  }

  public toggleMute(): boolean {
    this.isMuted = !this.isMuted;
    if (typeof window !== 'undefined') {
      localStorage.setItem('nscry_sound_enabled', (!this.isMuted).toString());
    }
    if (!this.isMuted) {
      this.initContext();
      this.playArcaneChime(660);
    }
    return !this.isMuted;
  }

  public getIsMuted(): boolean {
    return this.isMuted;
  }

  /**
   * Water Droplet / Ripple Resonance (sine wave sweep into low-pass echo)
   */
  public playWaterDrop(baseFreq = 800) {
    if (this.isMuted) return;
    this.initContext();
    if (!this.ctx) return;

    const osc = this.ctx.createOscillator();
    const gain = this.ctx.createGain();

    osc.type = 'sine';
    const now = this.ctx.currentTime;

    osc.frequency.setValueAtTime(baseFreq, now);
    osc.frequency.exponentialRampToValueAtTime(baseFreq * 2.2, now + 0.08);
    osc.frequency.exponentialRampToValueAtTime(baseFreq * 0.7, now + 0.35);

    gain.gain.setValueAtTime(0.2, now);
    gain.gain.exponentialRampToValueAtTime(0.001, now + 0.4);

    osc.connect(gain);
    gain.connect(this.ctx.destination);

    osc.start(now);
    osc.stop(now + 0.45);
  }

  /**
   * Arcane Crystal Chime (Harmonic frequencies for spell gems)
   */
  public playArcaneChime(rootFreq = 523.25) {
    if (this.isMuted) return;
    this.initContext();
    if (!this.ctx) return;

    const now = this.ctx.currentTime;
    const freqs = [rootFreq, rootFreq * 1.5, rootFreq * 2.0, rootFreq * 2.75];

    freqs.forEach((f, idx) => {
      const osc = this.ctx!.createOscillator();
      const gain = this.ctx!.createGain();

      osc.type = 'triangle';
      osc.frequency.setValueAtTime(f, now + idx * 0.04);

      gain.gain.setValueAtTime(0.08 / (idx + 1), now + idx * 0.04);
      gain.gain.exponentialRampToValueAtTime(0.0001, now + 0.8 + idx * 0.15);

      osc.connect(gain);
      gain.connect(this.ctx!.destination);

      osc.start(now + idx * 0.04);
      osc.stop(now + 1.2);
    });
  }

  /**
   * Tactile Brass Click (Crisp percussive burst for lens & socket interaction)
   */
  public playBrassClick() {
    if (this.isMuted) return;
    this.initContext();
    if (!this.ctx) return;

    const now = this.ctx.currentTime;

    // High pitched metal ring
    const osc = this.ctx.createOscillator();
    const gain = this.ctx.createGain();
    osc.type = 'sine';
    osc.frequency.setValueAtTime(2400, now);
    osc.frequency.exponentialRampToValueAtTime(800, now + 0.06);

    gain.gain.setValueAtTime(0.15, now);
    gain.gain.exponentialRampToValueAtTime(0.001, now + 0.08);

    osc.connect(gain);
    gain.connect(this.ctx.destination);

    osc.start(now);
    osc.stop(now + 0.1);
  }

  /**
   * Parchment Whisper (Filtered noise for scroll / page interaction)
   */
  public playParchment() {
    if (this.isMuted) return;
    this.initContext();
    if (!this.ctx) return;

    const bufferSize = this.ctx.sampleRate * 0.15;
    const buffer = this.ctx.createBuffer(1, bufferSize, this.ctx.sampleRate);
    const data = buffer.getChannelData(0);
    for (let i = 0; i < bufferSize; i++) {
      data[i] = Math.random() * 2 - 1;
    }

    const noise = this.ctx.createBufferSource();
    noise.buffer = buffer;

    const filter = this.ctx.createBiquadFilter();
    filter.type = 'bandpass';
    filter.frequency.value = 1200;
    filter.Q.value = 1.8;

    const gain = this.ctx.createGain();
    const now = this.ctx.currentTime;
    gain.gain.setValueAtTime(0.06, now);
    gain.gain.exponentialRampToValueAtTime(0.001, now + 0.15);

    noise.connect(filter);
    filter.connect(gain);
    gain.connect(this.ctx.destination);

    noise.start(now);
    noise.stop(now + 0.15);
  }
}

export const arcaneAudio = new ArcaneAudioEngine();
