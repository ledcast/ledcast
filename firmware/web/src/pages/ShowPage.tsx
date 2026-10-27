import {
  Banner,
  Button,
  Card,
  ColorSwatches,
  Crossfade,
  EmptyPage,
  Fieldset,
  Icon,
  Input,
  Page,
  SegmentedControl,
  Skeleton,
  Slider,
  Switch,
  Textarea,
} from "prime-ui-kit";
import { useRef, useState } from "react";

import { api } from "../app/api.ts";
import { ConnectionBanner } from "../app/ConnectionBanner.tsx";
import { DeviceStatus } from "../app/DeviceStatus.tsx";
import { usePoll } from "../app/status.ts";
import { SAVE_STATE_TEXT, useAutosave } from "../app/useAutosave.ts";
import styles from "./ShowPage.module.css";

type Show = {
  enabled: boolean;
  mode: "static" | "scroll";
  font_size_px: number;
  max_font_size_px?: number;
  speed_px_s: number;
  duration_s: number;
  color: string;
  text: string;
};

/** Цвета, которые светодиоды показывают чисто. */
const LED_COLORS = [
  { value: "#FFFFFF", label: "Белый" },
  { value: "#FF0000", label: "Красный" },
  { value: "#FF8000", label: "Оранжевый" },
  { value: "#FFFF00", label: "Жёлтый" },
  { value: "#00FF00", label: "Зелёный" },
  { value: "#00FFFF", label: "Голубой" },
  { value: "#0000FF", label: "Синий" },
  { value: "#FF00FF", label: "Пурпурный" },
];

const MAX_TEXT = 1024;

function phraseCount(text: string): number {
  return text.split("\n").filter((line) => line.trim()).length;
}

function phrasesText(count: number): string {
  const tens = count % 100;
  const ones = count % 10;
  if (tens >= 11 && tens <= 14) return `${count} фраз`;
  if (ones === 1) return `${count} фраза`;
  if (ones >= 2 && ones <= 4) return `${count} фразы`;
  return `${count} фраз`;
}

function normalize(data: Show): Show {
  const max = Number(data.max_font_size_px ?? 14);
  return {
    enabled: Boolean(data.enabled),
    mode: data.mode === "scroll" ? "scroll" : "static",
    font_size_px: Math.min(Number(data.font_size_px ?? 14), max),
    max_font_size_px: max,
    speed_px_s: Number(data.speed_px_s ?? 50),
    duration_s: Number(data.duration_s ?? 8),
    color: (data.color ?? "#FFFFFF").toUpperCase(),
    text: data.text ?? "",
  };
}

export function ShowPage() {
  const [show, setShow] = useState<Show | null>(null);
  const [loadState, setLoadState] = useState<"loading" | "ready" | "error">("loading");
  const [textKey, setTextKey] = useState(0);
  // Переключатель пишет сразу; автосохранение берёт его последнее значение, чтобы не вернуть старое.
  const enabled = useRef(false);

  const autosave = useAutosave<Show>(
    (value) => {
      const { max_font_size_px: _max, ...payload } = value;
      return api.put<Show>("/show", { ...payload, enabled: enabled.current });
    },
    (value) => value && setShow(normalize(value)),
  );

  const apply = (data: Show) => {
    const next = normalize(data);
    enabled.current = next.enabled;
    setShow((current) => {
      if (current && JSON.stringify(current) === JSON.stringify(next)) return current;
      if (current?.text !== next.text) setTextKey((key) => key + 1);
      return next;
    });
    setLoadState("ready");
  };

  usePoll(() => {
    if (autosave.isPending()) return;
    if (document.activeElement?.closest("[data-show-form]")) return;
    api
      .get<Show>("/show")
      .then((data) => (autosave.isPending() ? undefined : apply(data)))
      .catch(() => setLoadState((current) => (current === "ready" ? current : "error")));
  });

  const change = (patch: Partial<Show>) => {
    if (!show) return;
    const next = { ...show, ...patch };
    setShow(next);
    autosave.change(next);
  };

  const toggle = async (checked: boolean) => {
    await api.put("/show/enabled", { enabled: checked });
    enabled.current = checked;
    setShow((current) => (current ? { ...current, enabled: checked } : current));
  };

  const scrolling = show?.mode === "scroll";
  const colors = show && !LED_COLORS.some((item) => item.value === show.color)
    ? [...LED_COLORS, { value: show.color, label: "Текущий" }]
    : LED_COLORS;

  return (
    <Page.Root maxWidth="wide">
      <Page.TopBar>
        <Page.Heading>
          <Page.Title>Шоу</Page.Title>
          <Page.Description loading={!show}>
            {show
              ? [show.enabled ? "Показ включён" : "Показ выключен", phrasesText(phraseCount(show.text)), SAVE_STATE_TEXT[autosave.state]].join(" · ")
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
              <Banner.Title>Текст не применён</Banner.Title>
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
            <Card.Root>
              <Card.Body>
                <Skeleton shape="control" />
                <Skeleton shape="block" className={styles.textSkeleton} />
                <Skeleton shape="control" />
              </Card.Body>
            </Card.Root>
          ) : null}
          {loadState === "error" ? (
            <EmptyPage.Root tone="danger" aria-labelledby="show-error">
              <EmptyPage.Icon>
                <Icon name="status.danger" />
              </EmptyPage.Icon>
              <EmptyPage.Title id="show-error">Настройки показа не загрузились</EmptyPage.Title>
              <EmptyPage.Description>Контроллер не ответил. Проверьте, что он в сети.</EmptyPage.Description>
              <EmptyPage.Actions>
                <EmptyPage.RetryButton onRetry={() => api.get<Show>("/show").then(apply)} />
              </EmptyPage.Actions>
            </EmptyPage.Root>
          ) : null}
          {loadState === "ready" && show ? (
            <Card.Root data-show-form>
              <Card.Header>
                <Card.Title as="h2">Текст</Card.Title>
                <Card.Description>Каждая строка — отдельная фраза, пустые пропускаются</Card.Description>
              </Card.Header>
              <Card.Body>
                <Fieldset>
                  <Switch.Root
                    fullWidth
                    checked={show.enabled}
                    onCheckedChange={toggle}
                    hint="Строка показывает текст без NDI. Пока показ включён, видео на матрицу не идёт."
                  >
                    <Switch.Label>Автономный показ</Switch.Label>
                  </Switch.Root>
                  <Textarea.Root
                    key={textKey}
                    label="Фразы"
                    rows={8}
                    maxLength={MAX_TEXT}
                    placeholder={"ПОПКОРН\nТАТУ-ЗОНА"}
                    defaultValue={show.text}
                    counter={<Textarea.Counter current={show.text.length} max={MAX_TEXT} />}
                    onValueChange={(text) => change({ text })}
                  />
                  <Fieldset layout="pair">
                    <SegmentedControl.Root
                      label="Режим"
                      fullWidth
                      value={show.mode}
                      onValueChange={(mode) => change({ mode: mode === "scroll" ? "scroll" : "static" })}
                    >
                      <SegmentedControl.Item value="static">Статика</SegmentedControl.Item>
                      <SegmentedControl.Item value="scroll">Бегущая строка</SegmentedControl.Item>
                    </SegmentedControl.Root>
                    <Slider
                      label="Кегль"
                      min={7}
                      max={show.max_font_size_px ?? 14}
                      showValue
                      formatValue={(value) => `${value} px`}
                      key={`font-${show.font_size_px}`}
                      defaultValue={show.font_size_px}
                      onCommit={(font_size_px) => change({ font_size_px })}
                    />
                    {scrolling ? (
                      <Slider
                        label="Скорость"
                        min={1}
                        max={120}
                        showValue
                        formatValue={(value) => `${value} px/с`}
                        key={`speed-${show.speed_px_s}`}
                        defaultValue={show.speed_px_s}
                        onCommit={(speed_px_s) => change({ speed_px_s })}
                      />
                    ) : null}
                    <Input.Root
                      label="Длительность фразы"
                      hint={scrolling ? "0 — фразы бегут друг за другом" : undefined}
                      onCommit={(raw) => {
                        const value = Math.round(Number(raw));
                        const min = scrolling ? 0 : 1;
                        if (!Number.isFinite(value) || value < min || value > 3600) {
                          return Promise.reject(new Error(`От ${min} до 3600 секунд`));
                        }
                        change({ duration_s: value });
                        return undefined;
                      }}
                    >
                      <Input.Wrapper>
                        <Input.Field
                          key={show.duration_s}
                          type="number"
                          inputMode="numeric"
                          min={scrolling ? 0 : 1}
                          max={3600}
                          defaultValue={show.duration_s}
                        />
                        <Input.InlineAffix side="end">с</Input.InlineAffix>
                      </Input.Wrapper>
                    </Input.Root>
                    <ColorSwatches
                      label="Цвет"
                      presets={colors}
                      value={show.color}
                      onValueChange={(color) => {
                        if (color) change({ color });
                      }}
                    />
                  </Fieldset>
                </Fieldset>
              </Card.Body>
            </Card.Root>
          ) : null}
        </Crossfade>
      </Page.Body>
    </Page.Root>
  );
}
