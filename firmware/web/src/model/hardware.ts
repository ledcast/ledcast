// Модель линии HUB75: то, что отдаёт и принимает /api/v1/hardware, и правила выбора
// скана и развёртки по размеру модуля. Без React — чтобы проверять тестами.

export const MAX_MODULES = 24;

export type Module = {
  /** Номер модуля в цепи от контроллера, с единицы. */
  physical: number;
  rotate_180: boolean;
  mirror_x: boolean;
  mirror_y: boolean;
};

export type Hardware = {
  module: {
    width: number;
    height: number;
    scan_divisor: number;
    layout: string;
    color_order: string;
    shift_driver: string;
    line_decoder: string;
    oe_active_low: boolean;
    data_active_high: boolean;
  };
  module_count: number;
  /** Модули в порядке мест слева направо. */
  modules: Module[];
  display: {
    brightness: number;
    color_depth: number;
    dma_buffers: number;
    bus_hz: number;
    clk_phase: boolean;
    latch_blanking: number;
    frame_rate: number;
  };
};

export type Choice = { value: string; label: string };

export const MODULE_SIZES = ["32x16", "64x16", "32x32", "64x32", "80x40", "64x64", "128x64"];
export const CUSTOM_SIZE = "custom";

export function sizeKey(width: number, height: number): string {
  const key = `${width}x${height}`;
  return MODULE_SIZES.includes(key) ? key : CUSTOM_SIZE;
}

export function scanChoices(height: number): Choice[] {
  const items: Choice[] = [];
  if (height % 2 === 0) items.push({ value: String(height / 2), label: `1/${height / 2} · две строки одновременно` });
  if (height % 4 === 0) items.push({ value: String(height / 4), label: `1/${height / 4} · четыре строки одновременно` });
  return items;
}

const FOUR_SCAN: Record<number, string> = { 16: "four-scan-16", 32: "four-scan-32", 40: "four-scan-40", 64: "four-scan-64" };

const P10_LAYOUTS: Choice[] = [
  { value: "spiral", label: "Спиральная" },
  { value: "p10-quarter", label: "Четвертная" },
  { value: "p10-base8", label: "Блоки 8 · обратные" },
  { value: "p10-base8-plain", label: "Блоки 8" },
  { value: "p10-base16", label: "Блоки 16" },
  { value: "zstripe", label: "Z-полосы" },
  { value: "checkered", label: "Шахматная" },
];

export function layoutChoices(width: number, height: number, scan: number): Choice[] {
  const rows = height / scan;
  if (rows === 2) return [{ value: "auto", label: "Стандартная" }];
  if (rows !== 4) return [];
  const standard = FOUR_SCAN[height];
  const items: Choice[] = standard ? [{ value: standard, label: "Стандартная" }] : [];
  return width === 32 && height === 16 && scan === 4 ? [...items, ...P10_LAYOUTS] : items;
}

function pick(choices: Choice[], preferred: string | undefined): string {
  return choices.find((item) => item.value === preferred)?.value ?? choices[0]?.value ?? "";
}

/** Новый размер модуля: скан и развёртка переходят на допустимые для него значения. */
export function withModuleSize(hardware: Hardware, width: number, height: number): Hardware {
  const p10 = width === 32 && height === 16;
  const scan = Number(pick(scanChoices(height), String(p10 ? 4 : height / 2)));
  const layout = pick(layoutChoices(width, height, scan), p10 && scan === 4 ? "spiral" : undefined);
  return { ...hardware, module: { ...hardware.module, width, height, scan_divisor: scan, layout } };
}

export function withScan(hardware: Hardware, scan: number): Hardware {
  const { width, height } = hardware.module;
  const layout = pick(layoutChoices(width, height, scan), undefined);
  return { ...hardware, module: { ...hardware.module, scan_divisor: scan, layout } };
}

export function defaultModule(physical: number): Module {
  return { physical, rotate_180: false, mirror_x: false, mirror_y: false };
}

/** Заводской порядок: модуль 1 стоит у контроллера, справа. */
export function defaultOrder(count: number): Module[] {
  return Array.from({ length: count }, (_, index) => defaultModule(count - index));
}

function withModules(hardware: Hardware, modules: Module[]): Hardware {
  return { ...hardware, modules, module_count: modules.length };
}

/** Новый модуль становится последним в цепи и встаёт слева. */
export function addModule(hardware: Hardware): Hardware {
  if (hardware.modules.length >= MAX_MODULES) return hardware;
  return withModules(hardware, [defaultModule(hardware.modules.length + 1), ...hardware.modules]);
}

/** Номера цепи после удалённого сдвигаются, чтобы остаться сплошными. */
export function removeModule(hardware: Hardware, physical: number): Hardware {
  if (hardware.modules.length <= 1) return hardware;
  const modules = hardware.modules
    .filter((module) => module.physical !== physical)
    .map((module) => (module.physical > physical ? { ...module, physical: module.physical - 1 } : module));
  return withModules(hardware, modules);
}

export function updateModule(hardware: Hardware, physical: number, change: Partial<Module>): Hardware {
  return withModules(
    hardware,
    hardware.modules.map((module) => (module.physical === physical ? { ...module, ...change } : module)),
  );
}

/** Ориентация для всех: включает флаг, если он стоит не у всех, иначе снимает. */
export function toggleAll(hardware: Hardware, key: "rotate_180" | "mirror_y"): Hardware {
  const value = !hardware.modules.every((module) => module[key]);
  return withModules(hardware, hardware.modules.map((module) => ({ ...module, [key]: value })));
}

export function clearOrientation(hardware: Hardware): Hardware {
  return withModules(
    hardware,
    hardware.modules.map((module) => ({ ...module, rotate_180: false, mirror_x: false, mirror_y: false })),
  );
}

export function resetOrder(hardware: Hardware): Hardware {
  return withModules(hardware, defaultOrder(hardware.module_count));
}

export function reorder(hardware: Hardware, modules: Module[]): Hardware {
  return withModules(hardware, modules);
}

/** Ответ устройства → модель с числами и флагами строгих типов. */
export function normalizeHardware(data: Hardware): Hardware {
  const { module, display } = data;
  return {
    module: {
      width: Number(module.width),
      height: Number(module.height),
      scan_divisor: Number(module.scan_divisor),
      layout: module.layout,
      color_order: module.color_order,
      shift_driver: module.shift_driver,
      line_decoder: module.line_decoder,
      oe_active_low: Boolean(module.oe_active_low),
      data_active_high: Boolean(module.data_active_high),
    },
    module_count: Number(data.module_count),
    modules: data.modules.map((item) => ({
      physical: Number(item.physical),
      rotate_180: Boolean(item.rotate_180),
      mirror_x: Boolean(item.mirror_x),
      mirror_y: Boolean(item.mirror_y),
    })),
    display: {
      brightness: Number(display.brightness),
      color_depth: Number(display.color_depth),
      dma_buffers: Number(display.dma_buffers),
      bus_hz: Number(display.bus_hz),
      clk_phase: Boolean(display.clk_phase),
      latch_blanking: Number(display.latch_blanking),
      frame_rate: Number(display.frame_rate),
    },
  };
}
