import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";
import { viteSingleFile } from "vite-plugin-singlefile";

import { mockDevice } from "./dev/mock-device.ts";

// Контроллер отдаёт один gzip-файл со всем внутри: на площадке нет интернета.
// В разработке /api уходит на LEDCAST_DEVICE, а без него отвечает мок из dev/.
const device = process.env.LEDCAST_DEVICE;

export default defineConfig({
  plugins: [react(), viteSingleFile(), ...(device ? [] : [mockDevice()])],
  build: { target: "es2022", reportCompressedSize: false },
  server: device ? { proxy: { "/api": device } } : {},
});
