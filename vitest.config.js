import { defineConfig } from 'vitest/config';

export default defineConfig( {
	test: {
		environment: 'node',
		globals: true,
		// the sanity folder holds node:test scripts, not vitest suites
		exclude: [ '**/node_modules/**', 'sanity/**' ],
	},
} );
