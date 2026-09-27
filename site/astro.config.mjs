// @ts-check
import { defineConfig } from 'astro/config';

// https://astro.build/config
export default defineConfig({
  // Clean, extensionless URLs with no trailing slash (/about, not /about/ or
  // /about/index.html), and one .html file per route rather than a directory
  // per route -- matches how /web's own static build is verified to work
  // when relocated onto a plain static host (see scripts/verify-dist.sh).
  trailingSlash: 'never',
  build: {
    format: 'file',
  },
});
