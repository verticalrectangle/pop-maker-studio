import { defineConfig, type Plugin } from "vite";
import type { IncomingMessage, ServerResponse } from "node:http";

// COOP/COEP enable SharedArrayBuffer (threaded WASM fallback + fast worker comms).
const crossOriginIsolation: Plugin = {
  name: "cross-origin-isolation",
  configureServer(server) {
    server.middlewares.use(
      (_req: IncomingMessage, res: ServerResponse, next: () => void) => {
        res.setHeader("Cross-Origin-Opener-Policy", "same-origin");
        res.setHeader("Cross-Origin-Embedder-Policy", "require-corp");
        next();
      },
    );
  },
};

export default defineConfig({
  base: "./",
  plugins: [crossOriginIsolation],
  worker: { format: "es" },
  optimizeDeps: {
    exclude: ["@huggingface/transformers", "onnxruntime-web", "essentia.js"],
  },
  build: { target: "es2022" },
});
