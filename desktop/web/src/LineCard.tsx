import {
  Accordion,
  Badge,
  Button,
  Card,
  Fieldset,
  Icon,
  SegmentedControl,
  Select,
  Slider,
  StatusDot,
  Switch,
  Tooltip,
  Typography,
  useNotifications,
} from "prime-ui-kit";
import { useEffect, useRef, useState } from "react";

import { ndiLabel, ndiMachines, splitNdi } from "./ndi.ts";
import styles from "./LineCard.module.css";
import { PairLine } from "./PairLine.tsx";
import { watchPreview } from "./previews.ts";
import type { ConsoleStore } from "./store.ts";
import { type Fit, invoke, type LineNode, type Rotation } from "./tauri.ts";

const FITS: { value: Fit; label: string }[] = [
  { value: "Cover", label: "Заполнить" },
  { value: "Contain", label: "Вписать целиком" },
  { value: "Stretch", label: "Растянуть" },
];

const ROTATIONS: { value: Rotation; label: string }[] = [
  { value: "Deg0", label: "0°" },
  { value: "Deg90", label: "90°" },
  { value: "Deg180", label: "180°" },
  { value: "Deg270", label: "270°" },
];

const mbps = new Intl.NumberFormat("ru-RU", { minimumFractionDigits: 2, maximumFractionDigits: 2 });

function ago(seconds: number): string {
  if (seconds < 60) return `${seconds} с`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)} мин`;
  return `${Math.floor(seconds / 3600)} ч`;
}

const TEST_LABELS: Record<string, string> = {
  identify: "Позиции",
  grid: "Сетка",
  gray: "Шкала",
  red: "R",
  green: "G",
  blue: "B",
};

function testLabel(pattern: string): string {
  return TEST_LABELS[pattern] ?? pattern;
}

/** Состояние строки бейджем после названия. */
function stateBadge(node: LineNode, paused: boolean) {
  if (!node.online) {
    return (
      <Badge.Root tone="warning">
        {node.lastSeenSecs === null ? "Нет ответа" : `Не отвечает · ${ago(node.lastSeenSecs)}`}
      </Badge.Root>
    );
  }
  // Шоу и тест закрывают приём потока: ошибки отправки в этом режиме ожидаемы.
  if (node.testPattern) return <Badge.Root tone="warning">{`Тест · ${testLabel(node.testPattern)}`}</Badge.Root>;
  if (node.showEnabled) return <Badge.Root tone="success">Встроенное шоу</Badge.Root>;
  if (paused) return <Badge.Root tone="warning">На паузе</Badge.Root>;
  if (node.output.error) return <Badge.Root tone="danger">Ошибка отправки</Badge.Root>;
  if (node.output.running) return <Badge.Root tone="success">В эфире</Badge.Root>;
  return <Badge.Root tone="neutral">Ожидание</Badge.Root>;
}

/** Название в шапке: имя строки в сети роутера, иначе её собственное имя, иначе адрес. */
function title(node: LineNode): string {
  return node.routerName || node.name || node.host;
}

/** Где строка: адрес, размер, сигнал Wi‑Fi. */
function place(node: LineNode): string {
  return [node.host, `${node.width}\u00a0×\u00a0${node.height}\u00a0px`, node.rssi === null ? null : `${node.rssi}\u00a0dBm`]
    .filter(Boolean)
    .join(" · ");
}

/** Свёрнутые настройки одной строкой: что сейчас назначено. */
function settingsSummary(node: LineNode, sources: string[]): string {
  const { ndi_source, fit, transform } = node.mapping;
  return [
    ndi_source
      ? sources.includes(ndi_source)
        ? ndiLabel(ndi_source, ndiMachines([...sources, ndi_source]).length > 1)
        : `${ndiLabel(ndi_source, true)} — нет в сети`
      : "источник не назначен",
    FITS.find((item) => item.value === fit)?.label.toLowerCase(),
    ROTATIONS.find((item) => item.value === transform.rotation)?.label,
    transform.mirror_x ? "отражение" : null,
    `яркость ${node.brightness}`,
    node.showEnabled ? "встроенное шоу" : null,
  ]
    .filter(Boolean)
    .join(" · ");
}

type LineCardProps = {
  node: LineNode;
  sources: string[];
  store: ConsoleStore;
  /** Вывод на все строки остановлен кнопкой «Вывод». */
  paused: boolean;
};

/** Строка без доступа Console: только шапка и «Подключить», управлять ей нечем. */
function LockedLineCard({ node, store }: Pick<LineCardProps, "node" | "store">) {
  return (
    <Card.Root>
      <Card.Header>
        <Card.Title as="h2">{title(node)}</Card.Title>
        <Card.Badges>
          <Badge.Root tone="neutral">Не подключена</Badge.Root>
        </Card.Badges>
        <Card.Description>{place(node)}</Card.Description>
        <Card.Actions>
          <PairLine node={node} store={store} />
        </Card.Actions>
      </Card.Header>
    </Card.Root>
  );
}

export function LineCard(props: LineCardProps) {
  return props.node.paired ? <PairedLineCard {...props} /> : <LockedLineCard node={props.node} store={props.store} />;
}

function PairedLineCard({ node, sources, store, paused }: LineCardProps) {
  const { notify } = useNotifications();
  const canvas = useRef<HTMLCanvasElement>(null);
  const [hasFrame, setHasFrame] = useState(false);
  const mapping = node.mapping;
  const source = mapping.ndi_source ?? "";
  const sourceAvailable = sources.includes(source);
  const machines = ndiMachines(source ? [...sources, source] : sources);
  const manyMachines = machines.length > 1;
  const sourceItem = (name: string, disabled = false) => (
    <Select.Item key={name} value={name} label={ndiLabel(name, manyMachines)} keywords={name} disabled={disabled}>
      {splitNdi(name).source}
    </Select.Item>
  );

  useEffect(() => {
    const element = canvas.current;
    if (!element) return;
    const stop = watchPreview(node.id, element, () => setHasFrame(true));
    return () => {
      stop();
      setHasFrame(false);
    };
  }, [node.id, node.online]);

  const fail = (title: string) => (error: unknown) => {
    notify({ tone: "danger", title, description: String(error) });
  };
  // Контролы кита показывают значение сразу и ждут промис: отказ должен дойти до них, чтобы откатить.
  const failed = (title: string) => (error: unknown) => {
    fail(title)(error);
    throw error;
  };

  // Поток идёт на строку: шапка показывает, сколько кадров панель реально выводит и во что
  // это обходится сети. Панель заметно отстаёт от отправки — кадры теряются в пути.
  const streaming =
    node.online && !paused && !node.showEnabled && !node.testPattern && node.output.running && !node.output.error;
  const lagging = node.output.fps > 0 && node.deviceFps < node.output.fps - 2;

  const emptyText = !source
    ? "Выберите NDI-источник"
    : !sourceAvailable
      ? "Источник пропал из сети"
      : "Ждём первый кадр";

  return (
    <Card.Root>
      {/* Не отвечает — кадров нет и правки не дойдут до строки: хватает шапки с бейджем. */}
      {node.online ? (
        <Card.Media>
          <div className={styles.preview} data-frame={hasFrame || undefined}>
            <canvas ref={canvas} className={styles.canvas} width={node.width} height={node.height} aria-label={`Кадр на строке ${node.name || node.host}`} />
            {hasFrame ? null : (
              <Typography as="span" variant="body-s" tone="muted" className={styles.empty}>
                {emptyText}
              </Typography>
            )}
          </div>
        </Card.Media>
      ) : null}
      <Card.Header>
        <Card.Title as="h2">{title(node)}</Card.Title>
        <Card.Badges>
          {stateBadge(node, paused)}
          {streaming ? (
            <>
              <Badge.Root tone={lagging ? "warning" : "neutral"}>{`${Math.round(node.deviceFps)}\u00a0FPS`}</Badge.Root>
              <Badge.Root tone="neutral">{`${mbps.format(node.output.megabitsPerSecond)}\u00a0Мбит/с`}</Badge.Root>
              {node.output.clockSynced ? null : <Badge.Root tone="warning">Сверка часов</Badge.Root>}
            </>
          ) : null}
        </Card.Badges>
        <Card.Description>
          <StatusDot tone={node.online ? "success" : "danger"}>{node.online ? "Подключена" : "Не подключена"}</StatusDot>
          {place(node)}
        </Card.Description>
        <Card.Actions>
          <Tooltip.Root>
            <Tooltip.Trigger>
              <Button.Root
                variant="soft"
                tone="neutral"
                aria-label="Открыть контроллер"
                onClick={() => invoke("open_line_window", { nodeId: node.id }).catch(fail("Контроллер не открылся"))}
              >
                <Button.Icon>
                  <Icon name="action.openApp" />
                </Button.Icon>
              </Button.Root>
            </Tooltip.Trigger>
            <Tooltip.Content>Открыть контроллер</Tooltip.Content>
          </Tooltip.Root>
        </Card.Actions>
      </Card.Header>
      {node.online ? (
        <Card.Body>
          <Accordion.Root>
            <Accordion.Item value="settings">
              <Accordion.Header>
                <Accordion.Trigger>
                  Настройки
                  <Accordion.Description>{settingsSummary(node, sources)}</Accordion.Description>
                </Accordion.Trigger>
              </Accordion.Header>
              <Accordion.Content>
                <Fieldset>
                  {node.showControl && node.showEnabled !== null ? (
                    <SegmentedControl.Root
                      label="Что показывает строка"
                      fullWidth
                      value={node.showEnabled ? "show" : "ndi"}
                      hint={node.showEnabled ? "Строка показывает свой текст, NDI на неё не идёт" : undefined}
                      onValueChange={(value) =>
                        store.setShowEnabled(node, value === "show").catch(failed("Встроенное шоу не переключилось"))
                      }
                    >
                      <SegmentedControl.Item value="ndi">NDI</SegmentedControl.Item>
                      <SegmentedControl.Item value="show">Встроенное шоу</SegmentedControl.Item>
                    </SegmentedControl.Root>
                  ) : null}
                  <Fieldset layout="pair">
                    <Select.Root
                      label="NDI-источник"
                      placeholder={sources.length ? "Не назначен" : "Источники не найдены"}
                      disabled={!sources.length && !source}
                      clearable
                      value={source}
                      hint={source && !sourceAvailable ? "Источник пропал из сети — строка ждёт его" : undefined}
                      hintTone="warning"
                      onValueChange={(value: string) =>
                        store.updateMapping(node, { ...mapping, ndi_source: value || null }).catch(failed("Источник не назначен"))
                      }
                    >
                      <Select.Trigger>
                        <Select.Value />
                      </Select.Trigger>
                      <Select.Content searchable={sources.length > 6}>
                        {manyMachines
                          ? machines.map((machine) => (
                              <Select.Group key={machine} label={machine || "Без компьютера"}>
                                {sources.filter((name) => splitNdi(name).machine === machine).map((name) => sourceItem(name))}
                                {source && !sourceAvailable && splitNdi(source).machine === machine ? sourceItem(source, true) : null}
                              </Select.Group>
                            ))
                          : sources.map((name) => sourceItem(name))}
                        {!manyMachines && source && !sourceAvailable ? sourceItem(source, true) : null}
                      </Select.Content>
                    </Select.Root>
                    <Select.Root
                      label="Кадрирование"
                      value={mapping.fit}
                      onValueChange={(value: string) =>
                        store.updateMapping(node, { ...mapping, fit: value as Fit }).catch(failed("Кадрирование не применено"))
                      }
                    >
                      <Select.Trigger>
                        <Select.Value />
                      </Select.Trigger>
                      <Select.Content>
                        {FITS.map((fit) => (
                          <Select.Item key={fit.value} value={fit.value}>
                            {fit.label}
                          </Select.Item>
                        ))}
                      </Select.Content>
                    </Select.Root>
                    <SegmentedControl.Root
                      label="Поворот"
                      fullWidth
                      value={mapping.transform.rotation}
                      onValueChange={(value) =>
                        store
                          .updateMapping(node, { ...mapping, transform: { ...mapping.transform, rotation: value as Rotation } })
                          .catch(failed("Поворот не применён"))
                      }
                    >
                      {ROTATIONS.map((rotation) => (
                        <SegmentedControl.Item key={rotation.value} value={rotation.value}>
                          {rotation.label}
                        </SegmentedControl.Item>
                      ))}
                    </SegmentedControl.Root>
                    <Slider
                      label="Яркость"
                      min={0}
                      max={255}
                      showValue
                      value={node.brightness}
                      onValueChange={(value) => store.setBrightness(node, value)}
                      onCommit={(value) =>
                        invoke("update_brightness", { nodeId: node.id, value }).catch(failed("Яркость не применена"))
                      }
                    />
                  </Fieldset>
                  <Switch.Root
                    fullWidth
                    checked={mapping.transform.mirror_x}
                    onCheckedChange={(mirror_x) =>
                      store
                        .updateMapping(node, { ...mapping, transform: { ...mapping.transform, mirror_x } })
                        .catch(failed("Отражение не применено"))
                    }
                  >
                    <Switch.Label>Отразить по горизонтали</Switch.Label>
                  </Switch.Root>
                </Fieldset>
              </Accordion.Content>
            </Accordion.Item>
          </Accordion.Root>
          {node.output.error && !node.showEnabled && !node.testPattern ? (
            <Typography as="p" variant="body-s" tone="danger">
              {node.output.error}
            </Typography>
          ) : null}
        </Card.Body>
      ) : null}
    </Card.Root>
  );
}
