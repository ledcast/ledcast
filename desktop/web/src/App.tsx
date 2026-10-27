import {
  AppShell,
  Banner,
  Button,
  Card,
  Crossfade,
  EmptyPage,
  Icon,
  Page,
  Select,
  Skeleton,
  useNotifications,
} from "prime-ui-kit";
import { RadioIcon } from "prime-ui-kit/icons";
import { useEffect, useState, useSyncExternalStore } from "react";

import styles from "./App.module.css";
import { LineCard } from "./LineCard.tsx";
import { createConsoleStore } from "./store.ts";
import { invoke, type Snapshot } from "./tauri.ts";

const store = createConsoleStore(invoke);
const mbps = new Intl.NumberFormat("ru-RU", { minimumFractionDigits: 1, maximumFractionDigits: 1 });
/** Те же значения принимает бэкенд (`LATENCIES_MS` в feed.rs). */
const LATENCIES = [25, 50, 75, 100];

/** Строки, которым не хватает памяти на заданную задержку: из-за них вывод идёт с меньшей. */
function shortLines(snapshot: Snapshot): string[] {
  return snapshot.nodes
    .filter((node) => node.output.running && node.output.capacityMs > 0 && node.output.capacityMs < snapshot.latencyMs)
    .map((node) => node.routerName || node.name || node.host);
}

function linesText(count: number): string {
  const tens = count % 100;
  const ones = count % 10;
  if (tens >= 11 && tens <= 14) return `${count} строк`;
  if (ones === 1) return `${count} строка`;
  if (ones >= 2 && ones <= 4) return `${count} строки`;
  return `${count} строк`;
}

/** Подзаголовок одной длины в любом состоянии: от него зависит раскладка кнопок в шапке. */
function summary(snapshot: Snapshot): string {
  return [
    linesText(snapshot.nodes.length),
    snapshot.ndiError ? "NDI недоступен" : `NDI: ${snapshot.ndiSources.length}`,
    `${mbps.format(snapshot.running ? snapshot.trafficMbps : 0)}\u00a0Мбит/с`,
  ].join(" · ");
}

/** Снапшот раз в секунду, когда окно свободно от работы. */
function useSnapshotLoop() {
  useEffect(() => {
    let timer: ReturnType<typeof setTimeout>;
    let stopped = false;
    const idle = window.requestIdleCallback ?? ((callback: () => void) => setTimeout(callback, 0));
    const loop = () => {
      void store.refresh().finally(() => {
        if (!stopped) timer = setTimeout(() => idle(loop, { timeout: 1000 }), 1000);
      });
    };
    loop();
    return () => {
      stopped = true;
      clearTimeout(timer);
    };
  }, []);
}

export function App() {
  const snapshot = useSyncExternalStore(store.subscribe, store.getSnapshot);
  const { notify } = useNotifications();
  useSnapshotLoop();

  const fail = (title: string) => (error: unknown) => {
    notify({ tone: "danger", title, description: String(error) });
  };

  const discover = () => invoke("discover", { host: null }).then(store.refresh).catch(fail("Поиск не запустился"));

  // Кнопка переключается сразу, команда идёт в фоне; отказ возвращает прежнее состояние.
  const [runningWanted, setRunningWanted] = useState<boolean | null>(null);
  const running = runningWanted ?? snapshot?.running ?? false;
  const toggleOutput = () => {
    const next = !running;
    setRunningWanted(next);
    invoke(next ? "play" : "stop")
      .then(store.refresh)
      .catch(fail(next ? "Вывод не запущен" : "Вывод не остановлен"))
      .finally(() => setRunningWanted((current) => (current === next ? null : current)));
  };

  const state = !snapshot ? "loading" : snapshot.nodes.length ? "ready" : "empty";
  const limitedBy = snapshot && snapshot.running && snapshot.appliedLatencyMs < snapshot.latencyMs ? shortLines(snapshot) : [];

  return (
    <AppShell.Template fillViewport>
      <Page.Root>
        <Page.TopBar>
          <Page.Heading>
            <Page.Title>Строки</Page.Title>
            <Page.Description loading={!snapshot}>{snapshot ? summary(snapshot) : null}</Page.Description>
          </Page.Heading>
          <Page.Actions>
            <Select.Root
              aria-label="Задержка показа"
              disabled={!snapshot}
              value={snapshot ? String(snapshot.latencyMs) : ""}
              onValueChange={(value: string) =>
                store.setLatency(Number(value)).catch((error: unknown) => {
                  fail("Задержка не применена")(error);
                  throw error;
                })
              }
            >
              <Select.Trigger>
                <Select.Value />
              </Select.Trigger>
              <Select.Content>
                {LATENCIES.map((ms) => (
                  <Select.Item key={ms} value={String(ms)} label={`Задержка ${ms}\u00a0мс`}>
                    {`${ms}\u00a0мс`}
                  </Select.Item>
                ))}
              </Select.Content>
            </Select.Root>
            <Button.Root variant="soft" tone="neutral" loading={snapshot?.discovering} onClick={() => void discover()}>
              <Button.Icon>
                <Icon name="action.search" />
              </Button.Icon>
              Поиск
            </Button.Root>
            <Button.Root variant={running ? "soft" : "solid"} tone={running ? "neutral" : "accent"} disabled={!snapshot} onClick={toggleOutput}>
              <Button.Icon>
                <Icon name={running ? "action.pause" : "action.play"} />
              </Button.Icon>
              {running ? "Стоп" : "Старт"}
            </Button.Root>
          </Page.Actions>
        </Page.TopBar>
        <Page.Body>
          {snapshot?.ndiError ? (
            <Banner.Root tone="danger">
              <Banner.Content>
                <Banner.Title>NDI недоступен</Banner.Title>
                <Banner.Description>{snapshot.ndiError}</Banner.Description>
              </Banner.Content>
            </Banner.Root>
          ) : null}
          {snapshot && limitedBy.length ? (
            <Banner.Root tone="warning">
              <Banner.Content>
                <Banner.Title>{`Вывод идёт с задержкой ${snapshot.appliedLatencyMs}\u00a0мс вместо ${snapshot.latencyMs}`}</Banner.Title>
                <Banner.Description>
                  {`Строкам ${limitedBy.join(", ")} не хватает памяти на большую задержку, а она одна на все строки. Освободить память можно одинарным DMA-буфером в настройках строки — ценой разрывов кадра.`}
                </Banner.Description>
              </Banner.Content>
            </Banner.Root>
          ) : null}
          {snapshot?.notice ? (
            <Banner.Root tone="info">
              <Banner.Content>
                <Banner.Title>{snapshot.notice}</Banner.Title>
              </Banner.Content>
            </Banner.Root>
          ) : null}
          <Crossfade state={state} aria-busy={state === "loading"}>
            {state === "loading" ? (
              <div className={styles.lines}>
                {["a", "b"].map((key) => (
                  <Card.Root key={key}>
                    <Card.Body>
                      <Skeleton shape="block" className={styles.previewSkeleton} />
                      <Skeleton lines={2} />
                      <Skeleton shape="control" />
                    </Card.Body>
                  </Card.Root>
                ))}
              </div>
            ) : null}
            {state === "empty" && snapshot ? (
              <EmptyPage.Root layout="fill" aria-labelledby="lines-empty">
                <EmptyPage.Icon tone="accent">
                  <RadioIcon />
                </EmptyPage.Icon>
                <EmptyPage.Title id="lines-empty">
                  {snapshot.discovering ? "Ищем LEDCAST-строки" : "Строки не найдены"}
                </EmptyPage.Title>
                <EmptyPage.Description>
                  {snapshot.discovering
                    ? "Контроллеры в этой сети появятся сами."
                    : "Подключите контроллеры к этой сети: Console находит их сама."}
                </EmptyPage.Description>
                <EmptyPage.Actions>
                  <Button.Root loading={snapshot.discovering} onClick={() => void discover()}>
                    Найти строки
                  </Button.Root>
                </EmptyPage.Actions>
              </EmptyPage.Root>
            ) : null}
            {state === "ready" && snapshot ? (
              <div className={styles.lines}>
                {snapshot.nodes.map((node) => (
                  <LineCard key={node.id} node={node} sources={snapshot.ndiSources} store={store} paused={!running} />
                ))}
              </div>
            ) : null}
          </Crossfade>
        </Page.Body>
      </Page.Root>
    </AppShell.Template>
  );
}
