# PS 26147 — marketing/informational site

The landing page (`/`), `/about`, and `/docs` for PS 26147 (see the repo root
`CLAUDE.md`'s "Frontend / UI direction" for the aesthetic brief this follows,
and `DESIGN.md` for the research and palette/type/motif decisions). A separate,
content-focused Astro project from `/web` (the compiled app's static shell),
sharing the same design system (`src/styles/tokens.css`, copied verbatim into
`/web` in Phase 5 — edit it here, not there).

## Structure

```
site/
├── DESIGN.md              design-direction note (read this first)
├── src/
│   ├── styles/
│   │   ├── tokens.css      shared design tokens (color, type, spacing) — canonical copy
│   │   └── global.css      resets, base type, font loading, a11y (focus/reduced-motion)
│   ├── layouts/
│   │   └── BaseLayout.astro
│   ├── components/         Panel, Badge, Nav, Footer, BlinkCursor — the named motifs from DESIGN.md
│   └── pages/
│       ├── index.astro     landing page (placeholder until Phase 2)
│       ├── about.astro     (placeholder until Phase 3)
│       ├── docs.astro      (placeholder until Phase 4)
│       └── style-guide.astro   component showcase, not a real page (Phase 1 review deliverable)
└── public/
```

## Commands

| Command | Action |
| --- | --- |
| `npm install` | install dependencies |
| `npm run dev` | dev server at `localhost:4321` |
| `npm run build` | static build to `./dist/` |
| `npm run preview` | serve the build locally |

Static output, no server required — deploys the same way `/web` does.
