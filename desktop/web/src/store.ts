// Состояние Console: снапшот бэкенда раз в секунду и правки строк поверх него.
// Без React — чтобы проверять тестами; экран читает его через useSyncExternalStore.

import type { Invoke, LineNode, Mapping, Snapshot } from "./tauri.ts";

export type ConsoleStore = ReturnType<typeof createConsoleStore>;

export function createConsoleStore(invoke: Invoke) {
  let snapshot: Snapshot | null = null;
  let error: string | null = null;
  // Растёт при каждой подтверждаемой правке: снапшот, запрошенный до неё, уже устарел.
  let epoch = 0;
  const listeners = new Set<() => void>();

  const emit = () => {
    for (const listener of listeners) listener();
  };

  const patchNode = (id: string, patch: (node: LineNode) => LineNode) => {
    if (!snapshot) return;
    snapshot = { ...snapshot, nodes: snapshot.nodes.map((node) => (node.id === id ? patch(node) : node)) };
    emit();
  };

  async function refresh(): Promise<void> {
    const started = epoch;
    try {
      const next = await invoke("snapshot");
      if (started !== epoch) return;
      snapshot = next;
      error = null;
    } catch (reason) {
      if (started !== epoch) return;
      error = String(reason);
    }
    emit();
  }

  return {
    subscribe(listener: () => void) {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
    getSnapshot: () => snapshot,
    getError: () => error,
    refresh,

    /** Назначение источника и геометрии применяется сразу; бэкенд сохраняет его сам. */
    updateMapping(node: LineNode, mapping: Mapping): Promise<void> {
      patchNode(node.id, (current) => ({ ...current, mapping }));
      return invoke("update_mapping", {
        nodeId: node.id,
        ndiSource: mapping.ndi_source,
        fit: mapping.fit,
        rotation: mapping.transform.rotation,
        mirrorX: mapping.transform.mirror_x,
      });
    },

    /** Подключение паролем; карточка открывается сразу, не дожидаясь следующего снапшота. */
    async pairLine(node: LineNode, password: string): Promise<void> {
      await invoke("pair_line", { nodeId: node.id, password });
      // Снапшот, запрошенный до ответа, ещё видит строку закрытой.
      epoch++;
      patchNode(node.id, (current) => ({ ...current, paired: true }));
    },

    /** Задержка показа одна на все строки; применяется со следующего кадра. */
    setLatency(milliseconds: number): Promise<void> {
      if (snapshot) {
        snapshot = { ...snapshot, latencyMs: milliseconds };
        emit();
      }
      return invoke("set_latency", { milliseconds });
    },

    setBrightness(node: LineNode, value: number) {
      patchNode(node.id, (current) => ({ ...current, brightness: value }));
    },

    /**
     * Шоу включается только по ответу строки: на экране — то, что она подтвердила, а снапшот,
     * запрошенный до ответа, не возвращает старое значение.
     */
    async setShowEnabled(node: LineNode, enabled: boolean): Promise<void> {
      epoch++;
      try {
        const confirmed = await invoke("update_show_enabled", { nodeId: node.id, enabled });
        patchNode(node.id, (current) => ({ ...current, showEnabled: confirmed }));
        if (confirmed !== enabled) throw new Error("Строка не переключила Шоу");
      } finally {
        epoch++;
        void refresh();
      }
    },
  };
}
