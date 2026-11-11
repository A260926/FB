// Runner infrastructure only; the target tests are intentionally absent from the seed.
import fs from 'node:fs';
import Mocha from 'mocha';
const file = process.argv[2] || 'test/tests/solid_section_test.js';
if (!fs.existsSync (file)) { throw new Error (`Required target test file missing: ${file}`); }
const required = [
    'section-solid-half', 'section-cavity-and-island', 'section-seam-attributes',
    'section-instances-and-picking', 'section-export-counts', 'section-degenerate-and-reset'
];
const mocha = new Mocha ({ timeout : 10000, forbidPending : true, forbidOnly : true, reporter : 'spec' });
mocha.addFile (file);
await mocha.loadFilesAsync ();
const passed = new Set ();
const runner = mocha.run ((failures) => {
    const missing = required.filter ((title) => !passed.has (title));
    if (failures || missing.length || runner.stats.tests < required.length || runner.stats.pending) {
        console.error ('SECTION_TESTS_INCOMPLETE', JSON.stringify ({ failures, missing, stats : runner.stats }));
        process.exitCode = 1;
        return;
    }
    console.log (`SOLID_SECTION_OK tests=${runner.stats.tests} pending=${runner.stats.pending}`);
});
runner.on ('pass', (test) => passed.add (test.title));
