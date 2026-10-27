import assert from "node:assert/strict";
import { test } from "node:test";

import { createConsoleStore } from "./store.ts";
import type { Invoke, LineNode, Snapshot } from "./tauri.ts";

function deferred<T>() {
  let resolve!: (value: T) => void;
  let reject!: (reason: unknown) => void;
  const promise = new Promise<T>((a, b) => {
    resolve = a;
    reject = b;
  });
  return { promise, resolve, reject };
}

const node = { id: "node", showEnabled: false, showControl: true, online: true } as LineNode;
const snapshot = (showEnabled: boolean): Snapshot =>
  ({ nodes: [{ ...node, showEnabled }], ndiSources: [], running: true } as unknown as Snapshot);

test("Шоу: без оптимизма, ответ строки побеждает, старый снапшот не откатывает", async () => {
  const snapshots: Array<ReturnType<typeof deferred<Snapshot>>> = [];
  const toggles: Array<ReturnType<typeof deferred<boolean>>> = [];
  const invoke = ((command: string) => {
    if (command === "snapshot") {
      const next = deferred<Snapshot>();
      snapshots.push(next);
      return next.promise;
    }
    const next = deferred<boolean>();
    toggles.push(next);
    return next.promise;
  }) as Invoke;
  const store = createConsoleStore(invoke);

  const first = store.refresh();
  snapshots[0].resolve(snapshot(false));
  await first;
  const shown = () => store.getSnapshot()?.nodes[0].showEnabled;

  const stale = store.refresh();
  const toggle = store.setShowEnabled(node, true);
  assert.equal(shown(), false, "до ответа строки значение не меняется");
  snapshots[1].resolve(snapshot(false));
  await stale;
  toggles[0].resolve(true);
  await toggle;
  assert.equal(shown(), true, "снапшот, запрошенный до ответа, не откатывает подтверждение");

  const refused = store.setShowEnabled(node, false);
  toggles[1].resolve(true);
  await assert.rejects(refused, /не переключила/);
  assert.equal(shown(), true, "подтверждённое строкой значение остаётся");

  const failed = store.setShowEnabled(node, false);
  toggles[2].reject(new Error("offline"));
  await assert.rejects(failed, /offline/);
  assert.equal(shown(), true);
});

test("подключение: карточка открывается сразу, старый снапшот не закрывает её снова", async () => {
  const locked = { ...node, paired: false };
  const snapshots: Array<ReturnType<typeof deferred<Snapshot>>> = [];
  const pairing = deferred<void>();
  const invoke = ((command: string) => {
    if (command === "snapshot") {
      const next = deferred<Snapshot>();
      snapshots.push(next);
      return next.promise;
    }
    return pairing.promise;
  }) as Invoke;
  const store = createConsoleStore(invoke);
  const lockedSnapshot = { nodes: [locked], ndiSources: [], running: true } as unknown as Snapshot;

  const first = store.refresh();
  snapshots[0].resolve(lockedSnapshot);
  await first;
  const stale = store.refresh();
  const paired = store.pairLine(locked, "ledcast1");
  pairing.resolve();
  await paired;
  assert.equal(store.getSnapshot()?.nodes[0].paired, true);
  snapshots[1].resolve(lockedSnapshot);
  await stale;
  assert.equal(store.getSnapshot()?.nodes[0].paired, true);
});

test("подключение: отказ строки оставляет её закрытой и доходит до формы", async () => {
  const locked = { ...node, paired: false };
  const invoke = ((command: string) =>
    command === "snapshot"
      ? Promise.resolve({ nodes: [locked], ndiSources: [], running: true } as unknown as Snapshot)
      : Promise.reject("Неверный пароль")) as Invoke;
  const store = createConsoleStore(invoke);
  await store.refresh();
  await assert.rejects(store.pairLine(locked, "wrong"), (reason) => reason === "Неверный пароль");
  assert.equal(store.getSnapshot()?.nodes[0].paired, false);
});
