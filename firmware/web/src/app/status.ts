// Состояние контроллера из /api/v1/status: опрашивается всё время, пока открыта страница.

import { createContext, useCallback, useContext, useEffect, useRef, useState } from "react";

import type { NetworkStatus } from "../model/network.ts";
import { api } from "./api.ts";

export type Status = {
  id?: string;
  name?: string;
  firmware?: string;
  test_pattern?: string;
  show_enabled?: boolean;
  show_control?: boolean;
  modules?: number;
  width?: number;
  height?: number;
  uptime_s?: number;
  memory?: { free_heap?: number };
  network?: NetworkStatus;
  display?: { ready?: boolean; brightness?: number; frame_rate?: number; fps?: number; refresh_hz?: number; dma_bytes?: number };
  stream?: {
    signal?: boolean;
    fps?: number;
    received?: number;
    dropped?: number;
    source_ip?: string;
    /** Часы Console сверены: кадры встают одновременно на всех строках. */
    clock_synced?: boolean;
    /** Сколько кадров строка держит до показа. */
    reservoir_frames?: number;
  };
};

export type StatusState = {
  status: Status | null;
  /** Перечитать сейчас: после действия, которое меняет состояние. */
  refresh: () => Promise<void>;
};

export const StatusContext = createContext<StatusState>({ status: null, refresh: async () => {} });

export const useStatus = () => useContext(StatusContext);

const POLL_MS = 3000;

export function useStatusPolling(enabled: boolean): StatusState {
  const [status, setStatus] = useState<Status | null>(null);
  const sequence = useRef(0);

  const refresh = useCallback(async () => {
    const current = ++sequence.current;
    try {
      const next = await api.get<Status>("/status");
      if (current === sequence.current) setStatus(next);
    } catch {
      // Связь пропала: баннер говорит об этом, последние данные остаются на экране.
    }
  }, []);

  useEffect(() => {
    if (!enabled) return;
    void refresh();
    const timer = setInterval(() => void refresh(), POLL_MS);
    return () => clearInterval(timer);
  }, [enabled, refresh]);

  return { status, refresh };
}

/** Повторяет `load` каждые три секунды, пока экран открыт. */
/** Раздел на экране. Разделы смонтированы все сразу, как вкладки; спрятанный не опрашивает строку. */
export const SectionActiveContext = createContext(true);

/**
 * Загружает данные раздела при входе, даже спрятанного, чтобы переход был мгновенным; дальше
 * опрашивает строку, только пока раздел на экране, и сразу обновляет его при возвращении.
 */
export function usePoll(load: () => void) {
  const active = useContext(SectionActiveContext);
  const latest = useRef(load);
  latest.current = load;
  const loaded = useRef(false);
  useEffect(() => {
    if (active || !loaded.current) latest.current();
    loaded.current = true;
    if (!active) return;
    const timer = setInterval(() => latest.current(), POLL_MS);
    return () => clearInterval(timer);
  }, [active]);
}
