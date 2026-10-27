import { ButtonGroup, useNotifications } from "prime-ui-kit";

import { api } from "./api.ts";
import { useStatus } from "./status.ts";

export const TEST_GROUPS = [
  {
    label: "Разметка",
    patterns: [
      { value: "identify", label: "Позиции" },
      { value: "grid", label: "Сетка" },
      { value: "gray", label: "Шкала" },
    ],
  },
  {
    label: "Цвет",
    patterns: [
      { value: "red", label: "R" },
      { value: "green", label: "G" },
      { value: "blue", label: "B" },
    ],
  },
];

/** Тестовые изображения линии; повторное нажатие выключает тест. */
export function TestPatterns() {
  const { status, refresh } = useStatus();
  const { notify } = useNotifications();
  const active = status?.test_pattern ?? "";

  // Кит показывает нажатие сразу и ждёт промис; отказ возвращает сегмент как был.
  const run = (pattern: string, pressed: boolean) =>
    api
      .post("/test", pressed ? { pattern } : { release: true })
      .then(() => refresh())
      .catch((error: Error) => {
        notify({ tone: "danger", title: "Тест не запустился", description: error.message });
        throw error;
      });

  return (
    <>
      {TEST_GROUPS.map((group) => (
        <ButtonGroup.Root key={group.label} aria-label={group.label} fullWidth>
          {group.patterns.map((item) => (
            <ButtonGroup.Item
              key={item.value}
              pressed={active === item.value}
              onPressedChange={(pressed) => run(item.value, pressed)}
            >
              {item.label}
            </ButtonGroup.Item>
          ))}
        </ButtonGroup.Root>
      ))}
    </>
  );
}
