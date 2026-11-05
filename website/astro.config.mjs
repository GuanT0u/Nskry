import { defineConfig } from 'astro/config';

// No deployment identity or domain is assumed. Supports project Pages subpaths.
export default defineConfig({
  output: 'static',
  base: process.env.WEBSITE_BASE || '/',
  trailingSlash: 'always',
  devToolbar: { enabled: false },
});
