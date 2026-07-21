/**
 * Minimal ambient declarations for essentia.js subpath ES module entry points
 * used by the beats worker. The package ships no .d.ts for these paths; the
 * beats worker narrows the dynamic imports to local interfaces.
 */
declare module 'essentia.js/dist/essentia-wasm.es.js' {
  export const EssentiaWASM: unknown;
}
declare module 'essentia.js/dist/essentia.js-core.es.js' {
  // The default export is the Essentia constructor; typed locally in the worker.
  const Essentia: unknown;
  export default Essentia;
}
