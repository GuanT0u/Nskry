# Role & Objective
You are a creative frontend architect and interactive motion designer. Build an unforgettable, deeply immersive product showcase website for **Nscry** (pronounced "N-Scry").

## 1. Product Essence & Lore
- **Product Name**: **Nscry** (derived from *"Scry"* — the ancient mystical art of peering through a medium like water, crystal, or a mirror to observe distant or hidden realities in real time).
- **What Nscry Actually Is**: A hyper-lightweight Windows desktop utility built around two core philosophies:
  1. **Live Sub-Window Scrying (核心特色功能)**: Unlike traditional static screenshot tools, Nscry allows users to select a specific sub-region of a currently focused Windows application and spawn a floating, real-time live-mirroring viewport of that exact region (e.g., monitoring a compile log, a background video, or live telemetry while working elsewhere).
  2. **Zero-Bloat Modular Plugin Architecture (极致轻量 + 按需插件)**: The screenshot market is bloated with heavy all-in-one suites. Nscry ships as a featherweight core; users only equip the exact plugins they need (e.g., Live Region Pinning, OCR, Long Scrolling Capture, Lite Screen Recording).

## 2. Visual Direction: Diegetic Dark Fantasy (BG3 x Cinematic Scrollytelling)
- **Aesthetic**: Inspired by *Baldur's Gate 3* (BG3) and tactile dark-fantasy CRPG interfaces. **Reject generic SaaS templates**—no standard top navbars, no modern rounded white/glass cards, no generic hero-with-2-buttons layouts.
- **Diegetic UI (界面即世界)**: Every UI container, control, and transition should feel like a physical or magical artifact inside a dark gothic fantasy world—weathered mahogany, tarnished brass filigree, aged parchment, glowing arcane runes, and liquid mercury/water reflections.
- **Tech Stack Suggestion**: React, TypeScript, Tailwind CSS, GSAP (with ScrollTrigger), and Lenis for silky-smooth scroll choreography. Feel free to add custom Canvas particle systems, SVG displacement filters (water ripples/magic glow), and subtle synthesized Web Audio ambient feedback if it elevates the experience.

## 3. Asset Inventory (Located in `/public/assets/`)
Use the following pre-prepared visual assets seamlessly in the scene composition (and gracefully enhance or fallback with rich procedural SVG/CSS/Canvas effects if any asset is still loading):
- `/assets/hero-garden-bg.jpg` — Act 1 background: A misty, twilight gothic sanctuary garden shot from an oblique angle, featuring an ancient stone water basin.
- `/assets/scrying-well-topdown.png` — Act 1 zoom target (transparent PNG): Direct top-down view of the circular rune-carved stone scrying well/water mirror (center is transparent/translucent so live content shines through from beneath).
- `/assets/study-desk-bg.jpg` — Act 2 background: Top-down oblique view of a dark weathered mahogany/stone wizard's study desk with subtle candlelight.
- `/assets/open-grimoire.png` — Act 2 main canvas (transparent PNG): An open ancient leather-bound grimoire with brass corners and aged parchment pages.
- `/assets/brass-scrying-lens.png` — Interactive demo frame (transparent PNG): Ornate antique brass scrying lens with a hollow transparent center.
- `/assets/gem-pin.png`, `/assets/gem-ocr.png`, `/assets/gem-scroll.png`, `/assets/gem-record.png` — Modular plugin spell gems set in bronze sockets (transparent PNGs).

## 4. Interactive Choreography & 3-Act Experience
You have full creative freedom on exact visual polish and micro-interactions, provided you bring this 3-act narrative to life:

### Act 1: The Misty Sanctuary & Cinematic Dolly Zoom (Hero Section)
- **Initial State**: Full-bleed `hero-garden-bg.jpg` with subtle drifting mist particles and minimal, high-craft gothic typography introducing **Nscry** ("窥见幕后 · 轻量如风").
- **Scroll-Driven Dolly Zoom (希区柯克变焦运镜)**: As the user scrolls down:
  - A shimmering silver/arcane light sweeps across the garden's water basin.
  - Execute a dramatic spatial camera push/zoom: the oblique garden background compresses and recedes while `scrying-well-topdown.png` smoothly aligns to dead-center top-down perspective and scales up cleanly (ensure `transform-origin` and aspect ratios are locked so there is zero corner clipping or layout jump).
  - **The Scry Reveal**: Inside the hollow center of `scrying-well-topdown.png`, the water ripples away to reveal a **live, animated preview of a Windows sub-window monitor** (demonstrating Nscry peering into a live background app).
  - **The Plunge**: Scrolling further plunges the camera directly *through* the water surface (a smooth portal/ripple transition) into Act 2.

### Act 2: The Wizard's Study & Interactive Open Grimoire
- **Environment**: Transitions onto `study-desk-bg.jpg` with `open-grimoire.png` spread across the viewport like an interactive spellbook.
- **Interactive Scry Simulator**: Integrate `brass-scrying-lens.png` over an interactive mock Windows desktop canvas embedded in the grimoire/desk. Let visitors **drag the brass lens or draw a selection marquee** over different simulated background windows (e.g., a live terminal build, a radar/telemetry widget, or a video player) to experience how Nscry isolates and mirrors a sub-region in real time.
- **Diegetic Desk Props**: Place clickable/hoverable artifacts on the desk or grimoire margins (e.g., an enchanted quill or wax seal for Changelog/GitHub, a rune-etched download scroll for Windows x64).

### Act 3: D&D / BG3 Spell Slots — Modular Plugin Ecosystem
- Inside the grimoire's right page or an inlaid brass spell-rack, showcase the plugin philosophy ("Install only what you summon").
- Display `/assets/gem-pin.png`, `/assets/gem-ocr.png`, `/assets/gem-scroll.png`, and `/assets/gem-record.png` inside interactive spell sockets.
- **Interactive Equipping**: Clicking a spell gem activates/equips it with an arcane glow, rune inscription reveal, and a live "Core Weight / Memory Footprint" indicator (e.g., showing Nscry staying ultra-light at ~8MB base and only adding what you equip).

## 5. Quality & Engineering Principles
- Ensure silky 60fps scroll animations using GPU-accelerated transforms (`scale`, `translate3d`, `opacity`) and clean GSAP ScrollTrigger pinning/scrubbing.
- Maintain crisp readability of Chinese & English typography on parchment/dark surfaces using subtle ink-bleed text shadows or dark vignette backing where needed.
- Be bold, atmospheric, and inventive—treat this like an award-winning interactive web experience rather than a conventional software landing page.