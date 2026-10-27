// Имя NDI-источника — «КОМПЬЮТЕР (источник)». Когда все источники с одного компьютера, его имя —
// шум: показываем только источник. С разных компьютеров — группы по компьютеру.

export type NdiName = { machine: string; source: string };

export function splitNdi(name: string): NdiName {
  const match = /^(.*?) \((.*)\)$/.exec(name);
  if (!match) return { machine: "", source: name };
  // «MBPEUGENESURKOV.LAN» → «MBPEUGENESURKOV»: домен сети ничего не говорит.
  return { machine: match[1].split(".")[0], source: match[2] };
}

/** Компьютеры, с которых идут источники, в порядке первого появления. */
export function ndiMachines(names: string[]): string[] {
  return [...new Set(names.map((name) => splitNdi(name).machine))];
}

/** Подпись источника: с компьютером, только когда компьютеров несколько. */
export function ndiLabel(name: string, manyMachines: boolean): string {
  const { machine, source } = splitNdi(name);
  return manyMachines && machine ? `${source} · ${machine}` : source;
}
