import { useNotifications } from "prime-ui-kit";
import { useEffect, useRef, useState } from "react";

import { AutoSaver, type SaveState } from "../model/autosave.ts";

export const SAVE_STATE_TEXT: Record<SaveState, string> = {
  saved: "Сохранено",
  dirty: "Есть изменения",
  saving: "Применяю…",
  error: "Не применено",
};

/** Правки уходят на контроллер сами, через паузу после последней. */
export function useAutosave<T>(save: (value: T) => Promise<T | undefined>, onSaved: (value: T | undefined) => void) {
  const { notify } = useNotifications();
  const [state, setState] = useState<SaveState>("saved");
  const [error, setError] = useState<string>();
  const latest = useRef({ save, onSaved, notify });
  latest.current = { save, onSaved, notify };

  const [saver] = useState(
    () =>
      new AutoSaver<T>({
        delay: 450,
        save: (value) => latest.current.save(value),
        onSaved: (value) => latest.current.onSaved(value),
        onState: (next, failure) => {
          setState(next);
          if (next === "error" && failure) {
            setError(failure.message);
            latest.current.notify({ tone: "danger", title: "Изменения не применены", description: failure.message });
          } else if (next !== "error") {
            setError(undefined);
          }
        },
      }),
  );

  useEffect(() => () => saver.dispose(), [saver]);

  return {
    state,
    error,
    change: (value: T) => saver.change(value),
    retry: () => saver.flush(),
    /** Правки ещё не подтверждены: свежие данные опроса их не затирают. */
    isPending: () => saver.pending,
  };
}
