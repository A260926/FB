/** @satisfies {import("vitest/config").UserConfig} */
const config = {
    test: {
        /*
         * `task_support/` contains `node:test` contract tests run by
         * `task_support/target.sh`; keep them out of the vitest run.
         */
        exclude: ["task_support/**", "node_modules/**", "dist/**"],
    },
};

export default config;
