'use strict';
/*
 * validate.js -- check the wasm32 lane's modules (make build-wasm32).
 *
 * For each file given: the bytes validate as a WebAssembly module, the module
 * imports nothing (the library and tools/wasm/wasm32_libc.c are the whole
 * program; an import would be a host function the lane never provided), and
 * it instantiates against an empty import object.
 *
 * Usage: node tools/wasm/validate.js <module.wasm>...
 * Exit 0 when every module passes.
 */

const fs = require('fs');

let failed = 0;

for (const file of process.argv.slice(2)) {
  let bytes;
  try {
    bytes = fs.readFileSync(file);
  } catch (e) {
    console.error(file + ': cannot read: ' + e.message);
    failed++;
    continue;
  }
  if (!WebAssembly.validate(bytes)) {
    console.error(file + ': not a valid WebAssembly module');
    failed++;
    continue;
  }
  const mod = new WebAssembly.Module(bytes);
  const imports = WebAssembly.Module.imports(mod);
  if (imports.length !== 0) {
    console.error(file + ': ' + imports.length + ' import(s), first ' + imports[0].module + '.' + imports[0].name);
    failed++;
    continue;
  }
  try {
    new WebAssembly.Instance(mod, {});
  } catch (e) {
    console.error(file + ': does not instantiate: ' + e.message);
    failed++;
    continue;
  }
  console.log(file + ': valid, ' + WebAssembly.Module.exports(mod).length + ' exports, no imports');
}

process.exit(failed === 0 ? 0 : 1);
