import {
  Accordion,
  Badge,
  Banner,
  Button,
  Card,
  Crossfade,
  DescriptionList,
  Dropdown,
  EmptyPage,
  Fieldset,
  Icon,
  Input,
  Page,
  Select,
  Skeleton,
} from "prime-ui-kit";
import { useState } from "react";

import { api } from "../app/api.ts";
import { ConnectionBanner } from "../app/ConnectionBanner.tsx";
import { DeviceStatus } from "../app/DeviceStatus.tsx";
import { formatBytes, formatSize, pluralModules } from "../app/format.ts";
import { usePoll, useStatus } from "../app/status.ts";
import { SAVE_STATE_TEXT, useAutosave } from "../app/useAutosave.ts";
import {
  addModule,
  type Choice,
  clearOrientation,
  CUSTOM_SIZE,
  type Hardware,
  layoutChoices,
  MODULE_SIZES,
  normalizeHardware,
  removeModule,
  reorder,
  resetOrder,
  scanChoices,
  sizeKey,
  toggleAll,
  updateModule,
  withModuleSize,
  withScan,
} from "../model/hardware.ts";
import styles from "./HardwarePage.module.css";
import { ModuleOrder } from "./ModuleOrder.tsx";

const SIZE_CHOICES: Choice[] = [
  ...MODULE_SIZES.map((key) => ({ value: key, label: key.replace("x", " × ") })),
  { value: CUSTOM_SIZE, label: "Свой размер" },
];
const COLOR_ORDERS = ["RGB", "RBG", "GRB", "GBR", "BRG", "BGR"].map((value) => ({ value, label: value }));
const COLOR_DEPTHS = ["2", "3", "4", "5", "6", "7", "8"].map((value) => ({ value, label: `${value} бит` }));
const DMA_BUFFERS = [
  { value: "1", label: "Один" },
  { value: "2", label: "Два" },
];
const BUS_RATES = [
  { value: "16000000", label: "16 МГц" },
  { value: "20000000", label: "20 МГц" },
];
const SHIFT_DRIVERS = [
  { value: "generic", label: "Обычный · 74HC595" },
  { value: "icn2038s", label: "ICN2038S" },
  { value: "fm6124", label: "FM6124" },
  { value: "fm6126a", label: "FM6126A" },
  { value: "mbi5124", label: "MBI5124" },
  { value: "dp3246", label: "DP3246" },
];
const LINE_DECODERS = [
  { value: "138", label: "74HC138 · обычный" },
  { value: "direct", label: "Прямой" },
  { value: "sm5266p", label: "SM5266P" },
  { value: "sm5368", label: "SM5368" },
];
const CLOCK_PHASES = [
  { value: "true", label: "Инвертированная" },
  { value: "false", label: "Обычная" },
];
const DATA_POLARITY = [
  { value: "true", label: "Высокая" },
  { value: "false", label: "Низкая" },
];
const OE_POLARITY = [
  { value: "true", label: "Низкая" },
  { value: "false", label: "Высокая" },
];
type Status = "loading" | "ready" | "error";

function ChoiceField({
  label,
  value,
  choices,
  hint,
  onChange,
}: {
  label: string;
  value: string;
  choices: Choice[];
  hint?: string;
  onChange: (value: string) => void;
}) {
  return (
    <Select.Root label={label} hint={hint} value={value} onValueChange={(next: string) => {
      if (next) onChange(next);
    }} disabled={!choices.length}>
      <Select.Trigger>
        <Select.Value />
      </Select.Trigger>
      <Select.Content>
        {choices.map((choice) => (
          <Select.Item key={choice.value} value={choice.value}>
            {choice.label}
          </Select.Item>
        ))}
      </Select.Content>
    </Select.Root>
  );
}

/** Целое из поля: применяется по Enter и при уходе из поля, вне границ — подсказка. */
function NumberField({
  label,
  value,
  min,
  max,
  unit,
  onChange,
}: {
  label: string;
  value: number;
  min: number;
  max: number;
  unit?: string;
  onChange: (value: number) => void;
}) {
  const [error, setError] = useState<string>();
  return (
    <Input.Root
      label={label}
      error={error}
      onCommit={(raw) => {
        const next = Number(raw);
        if (!Number.isInteger(next) || next < min || next > max) {
          setError(`От ${min} до ${max}`);
          return Promise.reject(new Error("out of range"));
        }
        setError(undefined);
        onChange(next);
        return undefined;
      }}
    >
      <Input.Wrapper>
        <Input.Field key={value} type="number" inputMode="numeric" min={min} max={max} defaultValue={value} />
        {unit ? <Input.InlineAffix side="end">{unit}</Input.InlineAffix> : null}
      </Input.Wrapper>
    </Input.Root>
  );
}

export function HardwarePage() {
  const { status, refresh } = useStatus();
  const [hardware, setHardware] = useState<Hardware | null>(null);
  const [loadState, setLoadState] = useState<Status>("loading");
  const [customSize, setCustomSize] = useState(false);

  const autosave = useAutosave<Hardware>(
    async (value) => {
      try {
        return await api.put<Hardware>("/hardware", value);
      } finally {
        void refresh();
      }
    },
    (value) => value && setHardware(normalizeHardware(value)),
  );

  const load = () => {
    if (autosave.isPending()) return;
    api
      .get<Hardware>("/hardware")
      .then((data) => {
        if (autosave.isPending()) return;
        const next = normalizeHardware(data);
        setHardware((current) => (JSON.stringify(current) === JSON.stringify(next) ? current : next));
        setLoadState("ready");
      })
      .catch(() => setLoadState((current) => (current === "ready" ? current : "error")));
  };
  usePoll(load);

  const change = (next: Hardware) => {
    setHardware(next);
    autosave.change(next);
  };

  const display = status?.display;
  const module = hardware?.module;
  const sizeValue = module ? (customSize ? CUSTOM_SIZE : sizeKey(module.width, module.height)) : CUSTOM_SIZE;
  const showCustom = sizeValue === CUSTOM_SIZE;

  return (
    <Page.Root>
      <Page.TopBar>
        <Page.Heading>
          <Page.Title>Оборудование</Page.Title>
          <Page.Description loading={!hardware}>
            {hardware
              ? [
                  formatSize(hardware.module.width * hardware.module_count, hardware.module.height),
                  pluralModules(hardware.module_count),
                  SAVE_STATE_TEXT[autosave.state],
                ].join(" · ")
              : null}
          </Page.Description>
        </Page.Heading>
        <Page.Actions>
          <DeviceStatus />
        </Page.Actions>
      </Page.TopBar>
      <Page.Body>
        <ConnectionBanner />
        {autosave.state === "error" ? (
          <Banner.Root tone="danger">
            <Banner.Content>
              <Banner.Title>Изменения не применены</Banner.Title>
              <Banner.Description>{autosave.error}</Banner.Description>
              <Banner.Actions>
                <Button.Root variant="soft" tone="neutral" onClick={autosave.retry}>
                  Повторить
                </Button.Root>
              </Banner.Actions>
            </Banner.Content>
          </Banner.Root>
        ) : null}
        <Crossfade state={loadState} aria-busy={loadState === "loading"}>
          {loadState === "loading" ? (
            <div className={styles.columns}>
              <div className={styles.main}>
                <Card.Root>
                  <Card.Body>
                    <Skeleton lines={3} />
                    <Fieldset layout="pair">
                      <Skeleton shape="control" />
                      <Skeleton shape="control" />
                      <Skeleton shape="control" />
                    </Fieldset>
                  </Card.Body>
                </Card.Root>
              </div>
              <div className={styles.side}>
                <Card.Root>
                  <Card.Body>
                    <Skeleton lines={4} />
                  </Card.Body>
                </Card.Root>
              </div>
            </div>
          ) : null}
          {loadState === "error" ? (
            <EmptyPage.Root tone="danger" aria-labelledby="hardware-error">
              <EmptyPage.Icon>
                <Icon name="status.danger" />
              </EmptyPage.Icon>
              <EmptyPage.Title id="hardware-error">Настройки оборудования не загрузились</EmptyPage.Title>
              <EmptyPage.Description>Контроллер не ответил. Проверьте, что он в сети.</EmptyPage.Description>
              <EmptyPage.Actions>
                <EmptyPage.RetryButton
                  onRetry={() =>
                    api.get<Hardware>("/hardware").then((data) => {
                      setHardware(normalizeHardware(data));
                      setLoadState("ready");
                    })
                  }
                />
              </EmptyPage.Actions>
            </EmptyPage.Root>
          ) : null}
          {loadState === "ready" && hardware && module ? (
            <div className={styles.columns}>
              <div className={styles.main}>
                <Card.Root>
                  <Card.Header>
                    <Card.Title as="h2">Модули</Card.Title>
                    <Card.Description>Одинаковые модули в одной цепочке HUB75</Card.Description>
                  </Card.Header>
                  <Card.Body>
                    <Fieldset layout="pair">
                      <ChoiceField
                        label="Размер модуля"
                        value={sizeValue}
                        choices={SIZE_CHOICES}
                        onChange={(value) => {
                          if (value === CUSTOM_SIZE) {
                            setCustomSize(true);
                            return;
                          }
                          setCustomSize(false);
                          const [width, height] = value.split("x").map(Number);
                          change(withModuleSize(hardware, width, height));
                        }}
                      />
                      {showCustom ? (
                        <>
                          <NumberField
                            label="Ширина"
                            unit="px"
                            min={8}
                            max={256}
                            value={module.width}
                            onChange={(width) => change(withModuleSize(hardware, width, module.height))}
                          />
                          <NumberField
                            label="Высота"
                            unit="px"
                            min={4}
                            max={64}
                            value={module.height}
                            onChange={(height) => change(withModuleSize(hardware, module.width, height))}
                          />
                        </>
                      ) : null}
                      <ChoiceField
                        label="Скан"
                        value={String(module.scan_divisor)}
                        choices={scanChoices(module.height)}
                        onChange={(value) => change(withScan(hardware, Number(value)))}
                      />
                      <ChoiceField
                        label="Развёртка сигнала"
                        value={module.layout}
                        choices={layoutChoices(module.width, module.height, module.scan_divisor)}
                        onChange={(layout) => change({ ...hardware, module: { ...module, layout } })}
                      />
                      <ChoiceField
                        label="Порядок цвета"
                        value={module.color_order}
                        choices={COLOR_ORDERS}
                        onChange={(color_order) => change({ ...hardware, module: { ...module, color_order } })}
                      />
                    </Fieldset>
                    <Accordion.Root>
                      <Accordion.Item value="signal">
                        <Accordion.Header>
                          <Accordion.Trigger>
                            Параметры сигнала
                            <Accordion.Description>Драйвер, дешифратор, полярности и шина</Accordion.Description>
                          </Accordion.Trigger>
                        </Accordion.Header>
                        <Accordion.Content>
                          <Fieldset layout="pair">
                            <ChoiceField
                              label="Глубина цвета"
                              value={String(hardware.display.color_depth)}
                              choices={COLOR_DEPTHS}
                              onChange={(value) =>
                                change({ ...hardware, display: { ...hardware.display, color_depth: Number(value) } })
                              }
                            />
                            <ChoiceField
                              label="DMA-буферы"
                              value={String(hardware.display.dma_buffers)}
                              choices={DMA_BUFFERS}
                              onChange={(value) =>
                                change({ ...hardware, display: { ...hardware.display, dma_buffers: Number(value) } })
                              }
                            />
                            <ChoiceField
                              label="Частота шины"
                              value={String(hardware.display.bus_hz)}
                              choices={BUS_RATES}
                              onChange={(value) =>
                                change({ ...hardware, display: { ...hardware.display, bus_hz: Number(value) } })
                              }
                            />
                            <ChoiceField
                              label="Чип драйвера"
                              value={module.shift_driver}
                              choices={SHIFT_DRIVERS}
                              onChange={(shift_driver) => change({ ...hardware, module: { ...module, shift_driver } })}
                            />
                            <ChoiceField
                              label="Дешифратор строк"
                              value={module.line_decoder}
                              choices={LINE_DECODERS}
                              onChange={(line_decoder) => change({ ...hardware, module: { ...module, line_decoder } })}
                            />
                            <ChoiceField
                              label="Фаза тактового сигнала"
                              value={String(hardware.display.clk_phase)}
                              choices={CLOCK_PHASES}
                              onChange={(value) =>
                                change({ ...hardware, display: { ...hardware.display, clk_phase: value === "true" } })
                              }
                            />
                            <ChoiceField
                              label="Полярность данных"
                              value={String(module.data_active_high)}
                              choices={DATA_POLARITY}
                              onChange={(value) => change({ ...hardware, module: { ...module, data_active_high: value === "true" } })}
                            />
                            <ChoiceField
                              label="Полярность OE"
                              value={String(module.oe_active_low)}
                              choices={OE_POLARITY}
                              onChange={(value) => change({ ...hardware, module: { ...module, oe_active_low: value === "true" } })}
                            />
                            <NumberField
                              label="Latch blanking"
                              min={1}
                              max={4}
                              value={hardware.display.latch_blanking}
                              onChange={(latch_blanking) =>
                                change({ ...hardware, display: { ...hardware.display, latch_blanking } })
                              }
                            />
                          </Fieldset>
                        </Accordion.Content>
                      </Accordion.Item>
                    </Accordion.Root>
                  </Card.Body>
                </Card.Root>

                <Card.Root>
                  <Card.Header>
                    <Card.Title as="h2">Порядок строки</Card.Title>
                    <Card.Description>Слева направо · модуль 1 стоит у контроллера</Card.Description>
                    <Card.Actions>
                      <Dropdown.Root>
                        <Dropdown.Trigger>
                          <Button.Root variant="ghost" tone="neutral" aria-label="Все модули">
                            <Button.Icon>
                              <Icon name="action.more" />
                            </Button.Icon>
                          </Button.Root>
                        </Dropdown.Trigger>
                        <Dropdown.Content>
                          <Dropdown.Item onSelect={() => change(toggleAll(hardware, "rotate_180"))}>
                            Повернуть все на 180°
                          </Dropdown.Item>
                          <Dropdown.Item onSelect={() => change(toggleAll(hardware, "mirror_y"))}>
                            Отразить все по вертикали
                          </Dropdown.Item>
                          <Dropdown.Item onSelect={() => change(clearOrientation(hardware))}>
                            Сбросить ориентацию
                          </Dropdown.Item>
                          <Dropdown.Item onSelect={() => change(resetOrder(hardware))}>
                            Вернуть заводской порядок
                          </Dropdown.Item>
                        </Dropdown.Content>
                      </Dropdown.Root>
                    </Card.Actions>
                  </Card.Header>
                  <Card.Body>
                    <ModuleOrder
                      modules={hardware.modules}
                      onReorder={(modules) => change(reorder(hardware, modules))}
                      onToggle={(physical, key) =>
                        change(
                          updateModule(hardware, physical, {
                            [key]: !hardware.modules.find((item) => item.physical === physical)?.[key],
                          }),
                        )
                      }
                      onRemove={(physical) => change(removeModule(hardware, physical))}
                      onAdd={() => change(addModule(hardware))}
                    />
                  </Card.Body>
                </Card.Root>
              </div>

              <div className={styles.side}>
                <Card.Root>
                  <Card.Header>
                    <Card.Title as="h2">Вывод</Card.Title>
                    <Card.Badges>
                      {display?.ready === false ? (
                        <Badge.Root tone="danger">Ошибка</Badge.Root>
                      ) : (
                        <Badge.Root tone="success">Готов</Badge.Root>
                      )}
                    </Card.Badges>
                  </Card.Header>
                  <Card.Body>
                    <DescriptionList.Root align="end">
                      {status?.width && status.height && (status.width !== module.width * hardware.module_count || status.height !== module.height) ? (
                        <DescriptionList.Item label="Фактическое разрешение" tone="warning">
                          {formatSize(status.width, status.height)}
                        </DescriptionList.Item>
                      ) : null}
                      <DescriptionList.Item label="Обновление панели">
                        {display?.refresh_hz === undefined ? null : `${display.refresh_hz} Гц`}
                      </DescriptionList.Item>
                      <DescriptionList.Item label="Память DMA">{formatBytes(display?.dma_bytes)}</DescriptionList.Item>
                    </DescriptionList.Root>
                  </Card.Body>
                </Card.Root>
              </div>
            </div>
          ) : null}
        </Crossfade>
      </Page.Body>
    </Page.Root>
  );
}
