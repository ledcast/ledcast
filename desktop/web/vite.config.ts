import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";

// Tauri берёт готовую страницу из web/dist (tauri.conf.json → frontendDist).
export default defineConfig({
  plugins: [react()],
  build: { target: "safari16", reportCompressedSize: false },
  server: { port: 5182, strictPort: true },
});
