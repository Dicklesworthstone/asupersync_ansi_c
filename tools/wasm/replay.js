'use strict';
/*
 * replay.js -- run fixtures through the wasm32 replay module and print what
 * `asx-conformance replay` prints on the host for the same files
 * (make test-wasm32-replay, bd-9kll.15.4).
 *
 *   node tools/wasm/replay.js <replay.wasm> <fixture.json>...
 *
 * The module is freestanding (no imports): each fixture's bytes go into its
 * static input buffer, asx_wasm32_replay(len) runs the fixture's embedded
 * scenario through the C runtime compiled for wasm32, and the result line is
 * read back from its output buffer. One line per fixture, then
 * "replayed N fixture(s), M failed". Exit 0 when no fixture failed.
 */

const fs = require('fs');

function main(argv) {
  if (argv.length < 2) {
    console.error('usage: node replay.js <replay.wasm> <fixture.json>...');
    return 2;
  }
  const [wasmPath, ...fixtures] = argv;
  const module = new WebAssembly.Module(fs.readFileSync(wasmPath));
  if (WebAssembly.Module.imports(module).length !== 0) {
    console.error(wasmPath + ': the replay module must have no imports');
    return 2;
  }
  const x = new WebAssembly.Instance(module, {}).exports;
  for (const name of ['memory', 'asx_wasm32_input', 'asx_wasm32_input_capacity',
                      'asx_wasm32_output', 'asx_wasm32_output_capacity', 'asx_wasm32_replay']) {
    if (x[name] === undefined) {
      console.error(wasmPath + ': no export ' + name);
      return 2;
    }
  }
  const inputPtr = x.asx_wasm32_input();
  const inputCap = x.asx_wasm32_input_capacity();
  const outputPtr = x.asx_wasm32_output();
  const outputCap = x.asx_wasm32_output_capacity();
  const lines = [];
  let failed = 0;

  for (const path of fixtures) {
    const bytes = fs.readFileSync(path);
    if (bytes.length > inputCap) {
      console.error(path + ': ' + bytes.length + ' bytes, the module takes ' + inputCap);
      return 2;
    }
    // The memory may have grown during the previous run: take its buffer now.
    new Uint8Array(x.memory.buffer, inputPtr, bytes.length).set(bytes);
    const status = x.asx_wasm32_replay(bytes.length);
    const out = new Uint8Array(x.memory.buffer, outputPtr, outputCap);
    const end = out.indexOf(0);
    lines.push(Buffer.from(out.subarray(0, end < 0 ? outputCap : end)).toString('utf8'));
    if (status !== 0) failed++;
  }
  process.stdout.write(lines.join('\n') + (lines.length > 0 ? '\n' : ''));
  process.stdout.write('replayed ' + fixtures.length + ' fixture(s), ' + failed + ' failed\n');
  return failed === 0 ? 0 : 1;
}

process.exit(main(process.argv.slice(2)));
