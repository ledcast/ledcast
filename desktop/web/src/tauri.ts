// Команды бэкенда Console (desktop/src/console.rs). Tauri отдаёт их через window.__TAURI__.

export type Fit = "Cover" | "Contain" | "Stretch";
export type Rotation = "Deg0" | "Deg90" | "Deg180" | "Deg270";

export type Mapping = {
  node_id: string;
  ndi_source: string | null;
  fit: Fit;
  transform: { rotation: Rotation; mirror_x: boolean };
  enabled: boolean;
};

export type Output = {
  running: boolean;
  fps: number;
  megabitsPerSecond: number;
  error: string | null;
  sourceFps: number;
  /** Сколько задержки строка удерживает в резерве кадров; 0 — ещё не сообщила. */
  capacityMs: number;
  /** Строка сверила часы с Console и ставит кадры одновременно с остальными. */
  clockSynced: boolean;
};

export type LineNode = {
  id: string;
  name: string;
  host: string;
  width: number;
  height: number;
  modules: number;
  firmware: string;
  rssi: number | null;
  online: boolean;
  /** Console хранит ключ строки, и строка его принимает; иначе строку надо подключить паролем. */
  paired: boolean;
  /** Имя строки в DNS роутера (`ledcast-a4dc.lan`); `null` — роутер его не знает. */
  routerName: string | null;
  /** Секунд с последнего ответа; `null` — с запуска Console строка не отвечала. */
  lastSeenSecs: number | null;
  deviceFps: number;
  brightness: number;
  showEnabled: boolean | null;
  showControl: boolean;
  /** Тестовое изображение на строке; `null` — теста нет. */
  testPattern: string | null;
  mapping: Mapping;
  output: Output;
};

export type Snapshot = {
  nodes: LineNode[];
  ndiSources: string[];
  ndiVersion: string | null;
  ndiError: string | null;
  running: boolean;
  trafficMbps: number;
  discovering: boolean;
  notice: string | null;
  /** Заданная задержка показа, мс. */
  latencyMs: number;
  /** Задержка, с которой идёт вывод: меньше заданной, если строке не хватает памяти. */
  appliedLatencyMs: number;
};

export type Preview = { nodeId: string; width: number; height: number; pixels: number[]; generation: number };

type Commands = {
  snapshot: [Record<string, never>, Snapshot];
  discover: [{ host: string | null }, void];
  update_mapping: [{ nodeId: string; ndiSource: string | null; fit: Fit; rotation: Rotation; mirrorX: boolean }, void];
  update_brightness: [{ nodeId: string; value: number }, void];
  update_show_enabled: [{ nodeId: string; enabled: boolean }, boolean];
  play: [Record<string, never>, void];
  stop: [Record<string, never>, void];
  set_latency: [{ milliseconds: number }, void];
  open_web: [{ nodeId: string }, void];
  open_line_window: [{ nodeId: string }, void];
  pair_line: [{ nodeId: string; password: string }, void];
  forget_line: [{ nodeId: string }, void];
  preview: [{ nodeId: string; generation: number }, Preview | null];
};

export type Invoke = <C extends keyof Commands>(command: C, args?: Commands[C][0]) => Promise<Commands[C][1]>;

declare global {
  interface Window {
    __TAURI__?: { core: { invoke: Invoke } };
  }
}

let current: Invoke | null = window.__TAURI__?.core.invoke ?? null;

/** В браузере без Tauri (разработка) команды отвечает мок из dev/. */
export function setInvoke(invoke: Invoke) {
  current = invoke;
}

export const invoke: Invoke = (command, args) => {
  if (!current) return Promise.reject(new Error("Нет связи с бэкендом Console"));
  return current(command, args);
};
