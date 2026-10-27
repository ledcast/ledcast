// Автосохранение с паузой после правки: одна запись в полёте, правка во время записи
// уходит следом, ответ на устаревшую версию не затирает свежие правки.

export type SaveState = "saved" | "dirty" | "saving" | "error";

export type AutoSaverOptions<T> = {
  save: (value: T) => Promise<T | undefined>;
  /** Пауза после последней правки, мс. */
  delay: number;
  onState: (state: SaveState, error?: Error) => void;
  /** Ответ устройства на последнюю версию правок: его можно показать как есть. */
  onSaved: (value: T | undefined) => void;
  timers?: { set: (callback: () => void, ms: number) => unknown; clear: (handle: unknown) => void };
};

export class AutoSaver<T> {
  private revision = 0;
  private savedRevision = 0;
  private attemptRevision = 0;
  private saving = false;
  private timer: unknown = null;
  private latest: T | undefined;
  private readonly options: AutoSaverOptions<T>;
  private readonly timers: NonNullable<AutoSaverOptions<T>["timers"]>;

  constructor(options: AutoSaverOptions<T>) {
    this.options = options;
    this.timers = options.timers ?? {
      set: (callback, ms) => setTimeout(callback, ms),
      clear: (handle) => clearTimeout(handle as ReturnType<typeof setTimeout>),
    };
  }

  /** Есть правки, которых устройство ещё не подтвердило: опрос не должен их затирать. */
  get pending(): boolean {
    return this.saving || this.revision > this.savedRevision;
  }

  change(value: T): void {
    this.latest = value;
    this.revision++;
    this.options.onState("dirty");
    this.schedule(this.options.delay);
  }

  /** Записать сейчас, не дожидаясь паузы: повтор после ошибки. */
  flush(): void {
    if (this.revision > this.savedRevision) this.schedule(0);
  }

  dispose(): void {
    this.timers.clear(this.timer);
  }

  private schedule(ms: number): void {
    this.timers.clear(this.timer);
    this.timer = this.timers.set(() => this.run(), ms);
  }

  private run(): void {
    this.timer = null;
    if (this.saving || this.latest === undefined) return;
    const revision = this.revision;
    this.saving = true;
    this.attemptRevision = revision;
    this.options.onState("saving");
    this.options
      .save(this.latest)
      .then((result) => {
        this.savedRevision = revision;
        if (this.revision === revision) {
          this.options.onSaved(result);
          this.options.onState("saved");
        }
      })
      .catch((error: unknown) => {
        this.options.onState("error", error instanceof Error ? error : new Error(String(error)));
      })
      .finally(() => {
        this.saving = false;
        if (this.revision > this.attemptRevision) this.schedule(this.options.delay / 2);
      });
  }
}
