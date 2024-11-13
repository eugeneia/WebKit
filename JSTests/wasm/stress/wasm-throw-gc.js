//@ skip unless $isWasmPlatform

import { instantiate } from "../wabt-wrapper.js"
import * as assert from "../assert.js"

let watA = `
(module
    (tag $e)
    (func $functionA (export "functionA") (param $x i32) (result i32)
        try $ca (result)
            (if (i32.ge_u (local.get $x) (i32.const 1000))
                (then (return (local.get $x))))
            (throw $e)
        catch $e
            (return (call $functionA (i32.add (local.get $x) (i32.const 1))))
        end
        unreachable
    )
)
`

async function test() {
    const instanceA = await instantiate(watA, { o: {} }, { simd: true, exceptions: true })
    const { functionA } = instanceA.exports

    $vm.gc()
    console.log("STARTING TEST")
    for (let i = 0; i < 100000000; ++i) {
        assert.eq(functionA(1), 1000);
    }
}

await assert.asyncTest(test())
