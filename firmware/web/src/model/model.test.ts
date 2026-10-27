import assert from "node:assert/strict";
import { test } from "node:test";

import { AutoSaver, type SaveState } from "./autosave.ts";
import {
  addModule,
  defaultOrder,
  type Hardware,
  layoutChoices,
  removeModule,
  scanChoices,
  toggleAll,
  withModuleSize,
} from "./hardware.ts";
import { mergeNetworks } from "./network.ts";

function hardware(count: number): Hardware {
  return {
    module: {
      width: 32,
      height: 16,
      scan_divisor: 4,
      layout: "spiral",
      color_order: "RGB",
      shift_driver: "generic",
      line_decoder: "138",
      oe_active_low: true,
      data_active_high: true,
    },
    module_count: count,
    modules: defaultOrder(count),
    display: { brightness: 128, color_depth: 8, dma_buffers: 2, bus_hz: 20_000_000, clk_phase: true, latch_blanking: 1, frame_rate: 60 },
  };
}

test("скан и развёртка следуют размеру модуля", () => {
  assert.deepEqual(scanChoices(16).map((item) => item.value), ["8", "4"]);
  assert.deepEqual(scanChoices(40).map((item) => item.value), ["20", "10"]);
  assert.equal(layoutChoices(32, 16, 4).length, 8);
  assert.deepEqual(layoutChoices(64, 32, 16), [{ value: "auto", label: "Стандартная" }]);
  assert.deepEqual(layoutChoices(64, 32, 8).map((item) => item.value), ["four-scan-32"]);
  assert.deepEqual(layoutChoices(64, 48, 6), []);
  const big = withModuleSize(hardware(2), 64, 32);
  assert.equal(big.module.scan_divisor, 16);
  assert.equal(big.module.layout, "auto");
  const p10 = withModuleSize(big, 32, 16);
  assert.equal(p10.module.scan_divisor, 4);
  assert.equal(p10.module.layout, "spiral");
});

test("модули добавляются слева, номера цепи остаются сплошными", () => {
  const line = hardware(3);
  assert.deepEqual(line.modules.map((item) => item.physical), [3, 2, 1]);
  const added = addModule(line);
  assert.deepEqual(added.modules.map((item) => item.physical), [4, 3, 2, 1]);
  assert.equal(added.module_count, 4);
  const removed = removeModule(added, 2);
  assert.deepEqual(removed.modules.map((item) => item.physical), [3, 2, 1]);
  assert.equal(removeModule(hardware(1), 1).modules.length, 1);
});

test("ориентация для всех включается, пока стоит не у всех", () => {
  const line = hardware(2);
  const one = { ...line, modules: [{ ...line.modules[0], rotate_180: true }, line.modules[1]] };
  assert.equal(toggleAll(one, "rotate_180").modules.every((item) => item.rotate_180), true);
  assert.equal(toggleAll(toggleAll(one, "rotate_180"), "rotate_180").modules.some((item) => item.rotate_180), false);
});

test("сети сводятся в один список: подключённая, сохранённые, найденные, недоступные", () => {
  const scanned = [
    { ssid: "guest", rssi: -20 },
    { ssid: "home", rssi: -60 },
    { ssid: "guest", rssi: -80 },
    { ssid: "active", rssi: -70 },
    { ssid: "", rssi: -10 },
  ];
  const profiles = [{ ssid: "offline", id: "off" }, { ssid: "home", id: "saved-home" }, { ssid: "active" }];
  const result = mergeNetworks(scanned, profiles, { state: "sta", ssid: "active", rssi: -40 });
  assert.deepEqual(result.map((item) => item.ssid), ["active", "home", "guest", "offline"]);
  assert.equal(result[0].connected, true);
  assert.equal(result[0].rssi, -40);
  assert.equal(result[1].profileId, "saved-home");
  assert.equal(result[2].rssi, -20);
  assert.equal(result[3].available, false);
  assert.equal(mergeNetworks([], profiles, { state: "connecting", ssid: "active" }).some((item) => item.connected), false);
  assert.equal(mergeNetworks([], [], { state: "ap_sta", ssid: "current" })[0].connected, true);
});

function manualTimers() {
  const queue: Array<() => void> = [];
  return {
    timers: { set: (callback: () => void) => queue.push(callback), clear: () => queue.splice(0) },
    fire: () => queue.splice(0).forEach((callback) => callback()),
  };
}

const tick = () => new Promise((resolve) => setImmediate(resolve));

test("автосохранение: одна запись в полёте, свежая правка уходит следом, старый ответ не затирает", async () => {
  const { timers, fire } = manualTimers();
  const states: SaveState[] = [];
  const shown: Array<number | undefined> = [];
  const requests: Array<{ value: number; resolve: (value: number) => void; reject: (error: Error) => void }> = [];
  const saver = new AutoSaver<number>({
    delay: 400,
    timers,
    save: (value) => new Promise((resolve, reject) => requests.push({ value, resolve, reject })),
    onState: (state) => states.push(state),
    onSaved: (value) => shown.push(value),
  });

  saver.change(1);
  assert.equal(saver.pending, true);
  fire();
  assert.equal(requests.length, 1);
  saver.change(2);
  fire();
  assert.equal(requests.length, 1, "вторая запись ждёт первую");
  requests[0].resolve(1);
  await tick();
  assert.deepEqual(shown, [], "ответ на устаревшую версию не показывается");
  fire();
  assert.equal(requests[1].value, 2);
  requests[1].resolve(2);
  await tick();
  assert.deepEqual(shown, [2]);
  assert.equal(saver.pending, false);
  assert.equal(states.at(-1), "saved");

  saver.change(3);
  fire();
  requests[2].reject(new Error("offline"));
  await tick();
  assert.equal(states.at(-1), "error");
  assert.equal(saver.pending, true, "неподтверждённая правка держит опрос");
  saver.flush();
  fire();
  assert.equal(requests[3].value, 3);
});
