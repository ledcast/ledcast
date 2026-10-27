import { Badge, DescriptionList, Divider, Popover, SegmentedControl, Slider, Typography, useNotifications } from "prime-ui-kit";
import { useSyncExternalStore } from "react";

import { api, connection } from "./api.ts";
import styles from "./DeviceStatus.module.css";
import type { Hardware } from "../model/hardware.ts";
import { connectedSsid } from "../model/network.ts";
import { formatNumber } from "./format.ts";
import { useStatus } from "./status.ts";
import { TEST_GROUPS, TestPatterns } from "./TestPatterns.tsx";

type Tone = "success" | "warning" | "danger" | "neutral";

function testLabel(pattern: string): string {
  return TEST_GROUPS.flatMap((group) => group.patterns).find((item) => item.value === pattern)?.label ?? pattern;
}

/**
 * Чип состояния строки в шапке каждой страницы: режим, второй строкой — сеть Wi‑Fi и сила сигнала.
 * По нажатию — поповер: режим,
 * частота и яркость, затем видеопоток, редкие тесты линии последними. Остальное — на своих страницах.
 */
export function DeviceStatus() {
  const { status, refresh } = useStatus();
  const { notify } = useNotifications();
  const online = useSyncExternalStore(connection.subscribe, connection.get);

  const display = status?.display;
  const stream = status?.stream;

  const [tone, headline]: [Tone, string] = !online
    ? ["danger", "Нет связи"]
    : display?.ready === false
      ? ["danger", "Ошибка вывода"]
      : status?.test_pattern
        ? ["warning", `Тест · ${testLabel(status.test_pattern)}`]
        : status?.show_enabled
        ? ["success", "Шоу"]
        : stream?.signal
          ? ["success", `NDI · ${stream.fps ?? 0} к/с`]
          : ["neutral", "NDI · нет сигнала"];

  const ssid = online ? connectedSsid(status?.network) : null;
  const wifi = ssid ? [ssid, status?.network?.rssi === undefined ? null : `${status.network.rssi}\u00a0dBm`].filter(Boolean).join(" · ") : null;

  // Переключатели кита показывают выбор сразу и ждут промис: отказ возвращает прежнее значение.
  const failed = (title: string) => (error: Error) => {
    notify({ tone: "danger", title, description: error.message });
    throw error;
  };

  const setBrightness = (value: number) =>
    api
      .put("/brightness", { value })
      .then(() => refresh())
      .catch(failed("Яркость не применена"));

  // Частота — часть настроек оборудования: берём их свежими и меняем только её.
  const switchRate = (next: string) =>
    api
      .get<Hardware>("/hardware")
      .then((hardware) => api.put("/hardware", { ...hardware, display: { ...hardware.display, frame_rate: Number(next) } }))
      .then(() => refresh())
      .catch(failed("Частота не изменена"));

  const switchSource = (next: string) =>
    api
      .put("/show/enabled", { enabled: next === "show" })
      .then(() => refresh())
      .catch(failed("Источник не переключён"));

  return (
    <Popover.Root>
      <Popover.Trigger>
        <Badge.Root tone={tone} aria-label={`Состояние строки: ${[headline, wifi].filter(Boolean).join(", ")}`}>
          <Badge.Dot />
          {headline}
          {wifi ? <Badge.Description>{wifi}</Badge.Description> : null}
        </Badge.Root>
      </Popover.Trigger>
      <Popover.Content size="s" initialFocus aria-label="Состояние строки" className={styles.panel}>
        <SegmentedControl.Root
          label="Что показывает строка"
          fullWidth
          value={status?.show_enabled ? "show" : "ndi"}
          disabled={status?.show_control === false}
          onValueChange={switchSource}
        >
          <SegmentedControl.Item value="ndi">NDI</SegmentedControl.Item>
          <SegmentedControl.Item value="show">Шоу</SegmentedControl.Item>
        </SegmentedControl.Root>

        <SegmentedControl.Root
          label="Частота кадров"
          hint={display?.fps === undefined ? undefined : `Сейчас ${display.fps}\u00a0к/с`}
          fullWidth
          value={String(display?.frame_rate ?? 60)}
          onValueChange={switchRate}
        >
          <SegmentedControl.Item value="30">30 Гц</SegmentedControl.Item>
          <SegmentedControl.Item value="60">60 Гц</SegmentedControl.Item>
          <SegmentedControl.Item value="120">120 Гц</SegmentedControl.Item>
        </SegmentedControl.Root>

        <Slider
          label="Яркость"
          min={1}
          max={255}
          showValue
          key={display?.brightness}
          defaultValue={display?.brightness ?? 128}
          onCommit={setBrightness}
        />

        <Divider />
        <section className={styles.section} aria-labelledby="status-stream">
          <Typography as="h3" variant="title-xs" id="status-stream">
            Видеопоток
          </Typography>
          <DescriptionList.Root align="end">
            <DescriptionList.Item label="Источник">{stream?.signal ? stream.source_ip : null}</DescriptionList.Item>
            <DescriptionList.Item label="Потеряно" tone={stream?.dropped ? "warning" : undefined}>
              {stream?.dropped === undefined ? null : `${formatNumber(stream.dropped)} из ${formatNumber(stream.received ?? 0)}`}
            </DescriptionList.Item>
            <DescriptionList.Item label="Синхронизация" tone={stream?.signal && !stream.clock_synced ? "warning" : undefined}>
              {!stream?.signal
                ? null
                : stream.clock_synced
                  ? "по часам Console"
                  : "сверка часов"}
            </DescriptionList.Item>
            <DescriptionList.Item label="Резерв">
              {stream?.reservoir_frames ? `${formatNumber(stream.reservoir_frames)} кадров` : null}
            </DescriptionList.Item>
          </DescriptionList.Root>
        </section>

        <Divider />
        <section className={styles.section} aria-labelledby="status-test">
          <Typography as="h3" variant="title-xs" id="status-test">
            Проверка линии
          </Typography>
          <TestPatterns />
        </section>
      </Popover.Content>
    </Popover.Root>
  );
}
