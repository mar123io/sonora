import { defineConfig } from 'vite';

export default defineConfig({
  // Relative asset URLs, so the bundle resolves under sonora://app/ without
  // the scheme handler having to rewrite anything.
  base: './',
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    // Source maps in the development build only: they are useful in DevTools
    // and are both dead weight and a source leak in a shipped binary.
    sourcemap: process.env.NODE_ENV !== 'production',
    target: 'chrome120',
    rollupOptions: {
      output: {
        // One file each. The shell serves from memory, so there is nothing to
        // gain from chunking and something to lose in complexity.
        entryFileNames: 'assets/[name].js',
        chunkFileNames: 'assets/[name].js',
        assetFileNames: 'assets/[name].[ext]',
      },
    },
  },
});
