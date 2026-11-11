import { defineConfig, configDefaults } from 'vitest/config';

export default defineConfig( {
	test: {
		environment: 'node',
		globals: true,
		setupFiles: [ './test/matchers/toEqualBVH.js' ],
		// checks contains node:test based scripts rather than vitest suites
		exclude: [ ...configDefaults.exclude, 'checks/**' ],
	},
} );
