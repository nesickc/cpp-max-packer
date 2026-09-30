import { defineConfig } from 'vitest/config';

export default defineConfig({ test: { include: ['desktop/tests/**/*.test.ts', 'desktop/tests/**/*.test.tsx'], environment: 'jsdom' } });
