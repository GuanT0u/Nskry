/**
 * Atmospheric Mist & Arcane Motes Particle Engine
 * Pure 2D Canvas, high performance 60fps.
 */

interface Particle {
  x: number;
  y: number;
  vx: number;
  vy: number;
  radius: number;
  alpha: number;
  maxAlpha: number;
  color: string;
  isMote: boolean;
  pulsing: number;
}

export class MistParticles {
  private canvas: HTMLCanvasElement;
  private ctx: CanvasRenderingContext2D | null = null;
  private particles: Particle[] = [];
  private animFrameId: number | null = null;
  private isRunning = false;

  constructor(canvas: HTMLCanvasElement) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d');
    this.resize();
    this.initParticles();
    window.addEventListener('resize', () => this.resize());
  }

  private resize() {
    this.canvas.width = window.innerWidth;
    this.canvas.height = window.innerHeight;
  }

  private initParticles() {
    const count = Math.min(48, Math.floor(window.innerWidth / 30));
    this.particles = [];

    for (let i = 0; i < count; i++) {
      const isMote = Math.random() > 0.45;
      this.particles.push({
        x: Math.random() * this.canvas.width,
        y: Math.random() * this.canvas.height,
        vx: (Math.random() - 0.5) * (isMote ? 0.35 : 0.65),
        vy: -0.2 - Math.random() * (isMote ? 0.5 : 0.3),
        radius: isMote ? 1.5 + Math.random() * 2.5 : 45 + Math.random() * 95,
        alpha: Math.random() * 0.4,
        maxAlpha: isMote ? 0.7 + Math.random() * 0.3 : 0.08 + Math.random() * 0.09,
        color: isMote ? (Math.random() > 0.4 ? 'rgba(56, 189, 248, ' : 'rgba(234, 179, 8, ') : 'rgba(200, 225, 245, ',
        isMote,
        pulsing: Math.random() * Math.PI * 2,
      });
    }
  }

  public start() {
    if (this.isRunning) return;
    this.isRunning = true;
    const loop = () => {
      this.render();
      if (this.isRunning) {
        this.animFrameId = requestAnimationFrame(loop);
      }
    };
    loop();
  }

  public stop() {
    this.isRunning = false;
    if (this.animFrameId !== null) {
      cancelAnimationFrame(this.animFrameId);
      this.animFrameId = null;
    }
  }

  private render() {
    if (!this.ctx) return;
    this.ctx.clearRect(0, 0, this.canvas.width, this.canvas.height);

    for (const p of this.particles) {
      p.x += p.vx;
      p.y += p.vy;
      p.pulsing += 0.02;

      // Wrap around screen
      if (p.x < -100) p.x = this.canvas.width + 100;
      if (p.x > this.canvas.width + 100) p.x = -100;
      if (p.y < -100) {
        p.y = this.canvas.height + 50;
        p.x = Math.random() * this.canvas.width;
      }

      const dynamicAlpha = Math.sin(p.pulsing) * 0.2 + p.maxAlpha;
      const finalAlpha = Math.max(0.01, Math.min(1, dynamicAlpha));

      if (p.isMote) {
        // Glowing arcane sparkle
        this.ctx.beginPath();
        this.ctx.arc(p.x, p.y, p.radius, 0, Math.PI * 2);
        this.ctx.fillStyle = `${p.color}${finalAlpha})`;
        this.ctx.shadowBlur = 8;
        this.ctx.shadowColor = p.color === 'rgba(56, 189, 248, ' ? '#38bdf8' : '#eab308';
        this.ctx.fill();
        this.ctx.shadowBlur = 0;
      } else {
        // Soft drifting mist puff
        const grad = this.ctx.createRadialGradient(p.x, p.y, 0, p.x, p.y, p.radius);
        grad.addColorStop(0, `${p.color}${finalAlpha})`);
        grad.addColorStop(0.6, `${p.color}${finalAlpha * 0.4})`);
        grad.addColorStop(1, `${p.color}0)`);

        this.ctx.beginPath();
        this.ctx.arc(p.x, p.y, p.radius, 0, Math.PI * 2);
        this.ctx.fillStyle = grad;
        this.ctx.fill();
      }
    }
  }
}
