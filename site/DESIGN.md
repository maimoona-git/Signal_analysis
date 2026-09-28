> **Landing page update:** `/` now uses a separate black/white/grey direction (Archivo, big disc with a scrolling greyscale waterfall, modelled on a reference layout the user supplied). It lives in `src/styles/landing.css` and `src/layouts/LandingLayout.astro`. Everything below still describes `tokens.css`, `/web` and `/style-guide`.

# Design direction — PS 26147 site

Research notes and the resulting direction for `/site` (and, in Phase 5, `/web`).
Written before any HTML/CSS, per `CLAUDE.md`'s "Frontend / UI direction".

## What the research turned up

**Domain references (SDR/RF tooling):**
- [GNU Radio](https://www.gnuradio.org/), its [GRC editor](https://wiki.gnuradio.org/index.php/Guided_Tutorial_GRC),
  and the [Qt GUI sinks](https://www.gnuradio.org/doc/doxygen/page_qtgui.html) — the
  actual visual signature of this whole software family is the **flow-graph canvas**
  (bordered blocks, thin connector wires) and the **waterfall/FFT sink** (a
  scrolling spectrogram, usually on a dark background with a scientific colormap).
  The marketing chrome around these tools (gnuradio.org itself) is plain and
  underdesigned — logo, top nav, dense text — which is itself a useful signal: this
  audience does not expect (or even want) a flashy site, but it does recognize a
  flow-diagram/instrument-panel visual vocabulary instantly.
- [Gqrx](https://www.gqrx.dk/) and [SDR-Radio.com](https://www.sdr-radio.com/) —
  both plain, information-dense, screenshot-led pages (a big screenshot of the
  waterfall/FFT display *is* the hero image), alphabetically-organized nav for large
  feature sets, minimal color, sans-serif body text. Nothing decorative; substance
  over flourish is the house style of this whole category of software.
- **This project's own app** already has one real, established visual signature:
  `app/gui/viridis_table.inc` colors its waterfall with the viridis colormap (deep
  violet → teal → yellow-green). This was the initial idea for a signature site
  accent (see "Palette"'s revision note below for why it was dropped instead — the
  gradient runs through blue and purple, which the palette now avoids entirely).

**Retro references (early web / terminal):**
- Terminal/BBS aesthetics ([ANSI art](https://en.wikipedia.org/wiki/ANSI_art),
  monospace/bitmap type, command-line framing) and early-2000s tech-site conventions
  (bordered content boxes, dense hierarchical nav, monochrome-leaning palettes) come
  up again and again as the recognizable vocabulary of that era — table-layout
  clutter and animated GIFs do not need to come along for the ride.
- CRT/phosphor-monitor references (green phosphor `#00ff41`-ish, amber `#ff6a00`-ish,
  scanline overlays) are the most literal version of "old instrument" styling. Full
  scanline/glow effects read as costume rather than craft for a *tool's* marketing
  site — the useful takeaway is the **color family** (a single saturated phosphor
  accent against a dark field) and the **idea** of a monospace status readout, not
  the CRT curvature/glow gimmicks themselves.

## Palette

**Revised, mid-Phase-1, on direct instruction:** the first draft of this note
proposed a five-stop gradient lifted from the app's own viridis waterfall colormap
(`app/gui/viridis_table.inc`) as a signature "Waterfall Rule" accent. That colormap
runs purple → blue → teal → green, and the user explicitly rejected any
blue/navy/purple in the palette, plus asked for fewer colors overall and a more
restrained, old-school feel. The gradient motif is dropped entirely (not
just recolored) — see "Named motifs" below for what replaced it. The palette is
now strictly two families: near-black/warm-paper neutrals, plus muted green and
amber (the two classic phosphor-monitor colors), both dialled down from the first
draft's values so neither reads as a bright modern UI accent.

One dark, one light background are both used (dark for the hero/instrument sections,
light for long-form reading on `/about` and `/docs` — a whole site in inverted
phosphor-on-black would fight the "readable, accessible, modern" half of the brief).

| Token | Hex | Use |
| --- | --- | --- |
| `--ink-950` | `#0d1117` | primary dark background (hero, panel chrome, footer) |
| `--ink-900` | `#131a22` | raised dark surface (cards on dark bg) |
| `--paper-50` | `#f7f5f0` | primary light background (long-form pages) — warm off-white, not clinical `#fff`, closer to old equipment-manual paper |
| `--paper-100` | `#ece8df` | raised light surface (cards on light bg) |
| `--ink-text` | `#e8ecef` | body text on dark backgrounds |
| `--paper-text` | `#1b1f23` | body text on light backgrounds |
| `--line` | `#6b7684` (dark) / `#786e5a` (light) | hairline borders, grid lines |
| `--phosphor` | `#46916f` | primary accent — a dim, muted green (dialled back further than the first draft's `#3ddc84`; no neon, no bright saturated green) |
| `--phosphor-dim` | `#3a7856` | darker still — hover fills, rules |
| `--amber` | `#a37a3e` | secondary accent, used rarely — warnings, one badge variant |

Only two accent hues exist in the whole system, both warm/green phosphor-monitor
tones — no blue, no navy, no purple anywhere in the palette.

**Phase 6 polish, measured (not eyeballed):** computed actual WCAG contrast ratios
for every foreground/background pairing this system uses (sRGB relative luminance,
the standard formula) rather than assuming the retro palette was fine. `--ink-text`/
`--paper-text` on their own backgrounds both exceed 15:1 either way. Three real
failures turned up and were fixed by re-picking the value, not by changing where it's
used:
- `--line` (the Instrument Panel motif's own hairline border) measured 1.9:1 (dark)
  / 1.6:1 (light) against the page background it sits on — nowhere near WCAG 1.4.11's
  3:1 minimum for a UI component boundary, meaning the panels' signature borders were
  barely visible. Both re-picked to clear 4:1+.
- `--phosphor` and `--amber` are both text at least once (log lines, stage numbers,
  the primary button, an error message in `/web`'s shell) and a status dot at least
  once (`Badge`) — the dot has to work on `--surface-raised` in EITHER scheme, and the
  first draft's `--phosphor` measured only 2.7:1 against the light one. Both re-picked
  as the single value each that clears 4.5:1 (text, on the dark scheme, where all the
  text uses are) AND 3:1 (the dot, on both schemes' raised surface) at once, rather
  than adding a second scheme-specific shade for one component.
- The primary button used `--phosphor-dim` as its default (not just hover) fill with
  `--ink-950` text — 3.6:1, under the 4.5:1 normal-text minimum, on what is the
  single most important interactive element on the page. Moved to `--phosphor`
  (5.0:1) with an inverted hover state, instead of a still-failing hover-only fix.

This is exactly the retro-palette risk `CLAUDE.md` called out ahead of time — found
by computing it, the same way Feature 2/4 in the main app's own build found their
real statistical floors by measuring rather than assuming.

## Type

- **IBM Plex Mono** — headings, nav, labels, all-caps metadata tags ("SAMPLE RATE",
  "FEC: LDPC"), code. IBM Plex's own heritage is corporate hardware/engineering
  typography, which is a closer, more specific match to "instrument panel" than a
  generic geometric mono, and it is a real, complete, open family (regular through
  bold, true italics) rather than a display-only novelty face.
- **IBM Plex Sans** — body copy, long-form text on `/about` and `/docs`. Same family
  as the mono face (shared metrics, x-height, punctuation shapes), so pairing them
  never looks like two unrelated typefaces bolted together — a common failure mode
  when a "techy" mono is paired with an unrelated humanist sans.
- Type scale: a single modular scale (1.25 ratio) from a 16px base, defined once as
  CSS custom properties (`--text-xs` … `--text-3xl`) so `/site` and `/web` share
  identical steps.

## Named motifs

1. **Instrument panel** — bordered boxes (1px `--line`, sharp corners, no shadow)
   with a small monospace label tab overlapping the top-left border, mimicking a GRC
   block or a piece of rack equipment's silkscreened panel. The base "bordered
   panel" component from `CLAUDE.md`'s Phase 1 list. *From: GRC's flow-graph blocks,
   general instrument-panel silkscreening.*
2. **Scope grid** — a faint hairline grid (`--line` at low opacity) behind hero /
   section-break areas, evoking an oscilloscope graticule or the waterfall's own
   axis grid, never busy enough to fight body text laid over it. *From: GRC/Qt GUI
   waterfall and FFT sinks.*
3. **Signal rule** — a thin, single-color (`--phosphor-dim`) horizontal rule used as
   a section divider under key headings and atop the footer. Originally proposed as
   a five-color gradient lifted from the app's own viridis waterfall colormap; dropped
   for the flat single accent above on direct instruction (no blue/purple, fewer
   colors overall) — see "Palette"'s revision note.
4. **Status readout** — small monospace, uppercase, letter-spaced labels with a
   colored status dot (phosphor = good/active, amber = attention) for nav active
   states, build/test status callouts, and pipeline-stage indicators on the landing
   page. *From: terminal status lines / equipment indicator LEDs.*
5. **Blink cursor** — a `::after` blinking block-cursor (`▍`), used sparingly as a
   one-off detail (e.g. at the end of a hero headline, or a docs page's last updated
   line) via vanilla JS/CSS `@keyframes` — never as a page-wide gimmick. *From:
   terminal/BBS command-line framing.*

## What is explicitly NOT being done

- No scanline/CRT-curvature overlays, no glow/bloom text-shadow stacks, no
  blinking-everything, no marquee/table-layout pastiche. These read as costume, not
  craft, and fight the "modern touch" half of the brief.
- No screenshot-heavy, alphabetized mega-nav (SDR-Radio.com's approach) — this site
  has exactly three pages plus the app; density belongs on `/docs`, not the nav bar.
- Viridis is a signature accent, used once as a rule/underline motif — not a
  five-color UI palette.

## Sources

- [GNU Radio](https://www.gnuradio.org/)
- [GRC guided tutorial](https://wiki.gnuradio.org/index.php/Guided_Tutorial_GRC)
- [GNU Radio Qt GUI reference](https://www.gnuradio.org/doc/doxygen/page_qtgui.html)
- [Gqrx SDR](https://www.gqrx.dk/)
- [SDR-Radio.com](https://www.sdr-radio.com/)
- [ANSI art (Wikipedia)](https://en.wikipedia.org/wiki/ANSI_art)
- [Y2K aesthetic (Wikipedia)](https://en.wikipedia.org/wiki/Y2K_aesthetic)
- [CodeFronts: amber/green phosphor CSS reference](https://codefronts.com/design-styles/css-retro-designs/amber-and-green-phosphor-monitor/)
