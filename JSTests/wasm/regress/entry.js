import * as assert from '../assert.js';
import { instantiate } from '../wabt-wrapper.js';

async function test() {
    const instance = await instantiate(`
        (module
          (func $test (param) (result)
        )
        (export "test" (func $test)))
    `);

    for (let i = 0; i < 1e4; ++i) {
        assert.eq(instance.exports.test(), undefined);
    }
}

assert.asyncTest(test());
