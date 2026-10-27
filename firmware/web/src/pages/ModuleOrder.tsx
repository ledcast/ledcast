import { Button, ControlSizeProvider, Dnd, Icon, moveBefore, Tooltip, Typography, VisuallyHidden } from "prime-ui-kit";
import { RotateCwIcon } from "prime-ui-kit/icons";
import type { ReactNode } from "react";

import { MAX_MODULES, type Module } from "../model/hardware.ts";
import styles from "./ModuleOrder.module.css";

type ModuleOrderProps = {
  modules: Module[];
  onReorder: (modules: Module[]) => void;
  onToggle: (physical: number, key: "rotate_180" | "mirror_y") => void;
  onRemove: (physical: number) => void;
  onAdd: () => void;
};

const idOf = (module: Module) => String(module.physical);

function IconAction({ label, children, ...props }: { label: string; children: ReactNode } & Parameters<typeof Button.Root>[0]) {
  return (
    <Tooltip.Root>
      <Tooltip.Trigger>
        <Button.Root aria-label={label} {...props}>
          <Button.Icon>{children}</Button.Icon>
        </Button.Root>
      </Tooltip.Trigger>
      <Tooltip.Content>{label}</Tooltip.Content>
    </Tooltip.Root>
  );
}

/**
 * Модули в порядке мест слева направо: номер в цепи и ориентация. Плитку переносят мышью или
 * удержанием пальца, с клавиатуры — Alt + стрелки; кнопки на плитке не начинают перенос.
 */
export function ModuleOrder({ modules, onReorder, onToggle, onRemove, onAdd }: ModuleOrderProps) {
  // Плиток много: весь регион на тир xs.
  return (
    <ControlSizeProvider value="xs">
    <Dnd.Sortable
      as="ul"
      axis="grid"
      aria-label="Модули слева направо"
      className={styles.grid}
      items={modules}
      getId={idOf}
      getLabel={(module) => `Модуль ${module.physical}`}
      onReorder={(id, beforeId) => onReorder(moveBefore(modules, id, beforeId, idOf))}
      renderItem={(module, index) => (
        <Dnd.SortableItem
          id={idOf(module)}
          className={styles.tile}
          aria-labelledby={`module-${module.physical}`}
          aria-describedby={`module-${module.physical}-place`}
        >
          <div className={styles.head}>
            <Typography as="span" variant="title-m" numeric="tabular" id={`module-${module.physical}`} aria-label={`Модуль ${module.physical}`}>
              {module.physical}
            </Typography>
            <VisuallyHidden id={`module-${module.physical}-place`} data-dnd-mirror>
              место {index + 1} слева
            </VisuallyHidden>
            <IconAction
              label={`Удалить модуль ${module.physical}`}
              variant="ghost"
              tone="neutral"
              disabled={modules.length <= 1}
              onClick={() => onRemove(module.physical)}
            >
              <Icon name="action.close" />
            </IconAction>
          </div>
          <div className={styles.toggles}>
            <IconAction
              label="Повернуть на 180°"
              variant="soft"
              tone="neutral"
              pressed={module.rotate_180}
              onPressedChange={() => onToggle(module.physical, "rotate_180")}
            >
              <RotateCwIcon />
            </IconAction>
            <IconAction
              label="Отразить по вертикали"
              variant="soft"
              tone="neutral"
              pressed={module.mirror_y}
              onPressedChange={() => onToggle(module.physical, "mirror_y")}
            >
              <Icon name="action.flipVertical" />
            </IconAction>
          </div>
        </Dnd.SortableItem>
      )}
    >
      {modules.length < MAX_MODULES ? <Dnd.AddButton label="Добавить" aria-label="Добавить модуль слева" onClick={onAdd} /> : null}
    </Dnd.Sortable>
    </ControlSizeProvider>
  );
}
