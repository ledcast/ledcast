// Превью кадров: один цикл на все строки, новый кадр запрашивается, как только нарисован прошлый.

import { invoke } from "./tauri.ts";

const canvases = new Map<string, HTMLCanvasElement>();
const generations = new Map<string, number>();
const listeners = new Map<string, () => void>();
let running = false;

function draw(canvas: HTMLCanvasElement, width: number, height: number, pixels: number[]) {
  if (canvas.width !== width || canvas.height !== height) {
    canvas.width = width;
    canvas.height = height;
  }
  const rgba = new Uint8ClampedArray(width * height * 4);
  for (let i = 0, j = 0; i < pixels.length; i += 3, j += 4) {
    rgba[j] = pixels[i];
    rgba[j + 1] = pixels[i + 1];
    rgba[j + 2] = pixels[i + 2];
    rgba[j + 3] = 255;
  }
  canvas.getContext("2d")?.putImageData(new ImageData(rgba, width, height), 0, 0);
}

async function tick() {
  await Promise.all(
    [...canvases].map(async ([nodeId, canvas]) => {
      try {
        const preview = await invoke("preview", { nodeId, generation: generations.get(nodeId) ?? 0 });
        if (!preview) return;
        generations.set(nodeId, preview.generation);
        draw(canvas, preview.width, preview.height, preview.pixels);
        listeners.get(nodeId)?.();
      } catch {
        // Кадра нет — превью держит последний.
      }
    }),
  );
}

function loop() {
  if (!canvases.size) {
    running = false;
    return;
  }
  void tick().finally(() => requestAnimationFrame(loop));
}

/** Холст строки получает кадры, пока смонтирован; `onFrame` — когда пришёл первый и следующие. */
export function watchPreview(nodeId: string, canvas: HTMLCanvasElement, onFrame: () => void): () => void {
  canvases.set(nodeId, canvas);
  listeners.set(nodeId, onFrame);
  generations.delete(nodeId);
  if (!running) {
    running = true;
    requestAnimationFrame(loop);
  }
  return () => {
    canvases.delete(nodeId);
    listeners.delete(nodeId);
  };
}
