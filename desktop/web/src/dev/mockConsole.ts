// Бэкенд Console для разработки в браузере: три строки, источники NDI и живые превью.

import { type Invoke, type LineNode, type Snapshot, setInvoke } from "../tauri.ts";

const SOURCES = ["STUDIO-PC (Resolume Arena)", "STUDIO-PC (OBS Main)", "VJ-MAC (TouchDesigner)", "CAM-01 (NDI HX)"];

function line(id: string, name: string, host: string, modules: number, source: string | null, extra: Partial<LineNode> = {}): LineNode {
  return {
    id,
    name,
    host,
    width: 32 * modules,
    height: 16,
    modules,
    firmware: "0.1.0",
    rssi: -48 - modules,
    online: true,
    paired: true,
    lastSeenSecs: 0,
    deviceFps: source ? 49.8 : 0,
    brightness: 160,
    showEnabled: false,
    showControl: true,
    testPattern: null,
    routerName: null,
    mapping: { node_id: id, ndi_source: source, fit: "Cover", transform: { rotation: "Deg0", mirror_x: false }, enabled: true },
    output: {
      running: Boolean(source),
      fps: source ? 50 : 0,
      megabitsPerSecond: source ? 0.42 * modules : 0,
      error: null,
      sourceFps: source ? 50 : 0,
      capacityMs: source ? 280 - modules * 20 : 0,
      clockSynced: Boolean(source),
    },
    ...extra,
  };
}

const state: Snapshot = {
  nodes: [
    line("bar", "Бар · над стойкой", "192.168.1.47", 6, SOURCES[0]),
    line("stage", "Сцена · портал", "192.168.1.52", 8, SOURCES[1], { paired: false }),
    line("entry", "Вход", "192.168.1.61", 4, null, { online: false, lastSeenSecs: 340, rssi: null }),
  ],
  ndiSources: SOURCES,
  ndiVersion: "NDI SDK 6.1",
  ndiError: null,
  running: true,
  trafficMbps: 5.88,
  discovering: false,
  notice: null,
  latencyMs: 100,
  appliedLatencyMs: 100,
};

let generation = 0;

function frame(node: LineNode) {
  const { width, height } = node;
  const t = performance.now() / 1000;
  const pixels: number[] = new Array(width * height * 3);
  const hue = node.id === "bar" ? 0 : 2;
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const wave = Math.sin(x / 6 + t * 3 + hue) * 0.5 + 0.5;
      const band = Math.sin(y / 3 - t * 2) * 0.5 + 0.5;
      const i = (y * width + x) * 3;
      pixels[i] = Math.round(255 * wave * (node.brightness / 255));
      pixels[i + 1] = Math.round(120 * band * (node.brightness / 255));
      pixels[i + 2] = Math.round(255 * (1 - wave) * (node.brightness / 255));
    }
  }
  return { nodeId: node.id, width, height, pixels, generation: ++generation };
}

const wait = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

const find = (id: string) => {
  const node = state.nodes.find((item) => item.id === id);
  if (!node) throw new Error("Строка не найдена");
  return node;
};

const mock = (async (command: string, args: Record<string, unknown> = {}) => {
  switch (command) {
    case "snapshot":
      return structuredClone(state);
    case "preview": {
      await wait(40);
      const node = find(String(args.nodeId));
      return node.online && node.mapping.ndi_source && state.running ? frame(node) : null;
    }
    case "discover":
      state.discovering = true;
      setTimeout(() => {
        state.discovering = false;
      }, 2500);
      return undefined;
    case "update_mapping": {
      const node = find(String(args.nodeId));
      node.mapping = {
        ...node.mapping,
        ndi_source: (args.ndiSource as string | null) ?? null,
        fit: args.fit as LineNode["mapping"]["fit"],
        transform: { rotation: args.rotation as LineNode["mapping"]["transform"]["rotation"], mirror_x: Boolean(args.mirrorX) },
      };
      return undefined;
    }
    case "update_brightness":
      find(String(args.nodeId)).brightness = Number(args.value);
      return undefined;
    case "update_show_enabled": {
      await wait(600);
      const node = find(String(args.nodeId));
      node.showEnabled = Boolean(args.enabled);
      return node.showEnabled;
    }
    case "set_latency":
      state.latencyMs = Number(args.milliseconds);
      state.appliedLatencyMs = Math.min(state.latencyMs, ...state.nodes.filter((n) => n.output.capacityMs).map((n) => n.output.capacityMs));
      return undefined;
    case "play":
    case "stop":
      state.running = command === "play";
      return undefined;
    case "pair_line": {
      await wait(700);
      const node = find(String(args.nodeId));
      // Как бэкенд: неверный пароль — понятная строка, её показывает поле.
      if (args.password !== "ledcast1") throw "Неверный пароль";
      node.paired = true;
      return undefined;
    }
    case "forget_line":
      find(String(args.nodeId)).paired = false;
      return undefined;
    case "open_line_window":
    case "open_web":
      window.open(`http://${find(String(args.nodeId)).host}/`, "_blank");
      return undefined;
    default:
      throw new Error(`Нет такой команды: ${command}`);
  }
}) as Invoke;

export function installMockConsole() {
  setInvoke(mock);
}
