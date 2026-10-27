// Числа и единицы по-русски: разделители разрядов, неразрывный пробел перед единицей.

const integer = new Intl.NumberFormat("ru-RU");
const decimal = new Intl.NumberFormat("ru-RU", { maximumFractionDigits: 1 });

export const formatNumber = (value: number) => integer.format(value);

export function formatBytes(value: number | undefined): string | null {
  if (value === undefined) return null;
  if (value < 1024) return `${integer.format(value)} Б`;
  if (value < 1024 * 1024) return `${decimal.format(value / 1024)} КБ`;
  return `${decimal.format(value / 1024 / 1024)} МБ`;
}

export function formatDuration(seconds: number | undefined): string | null {
  if (seconds === undefined || !Number.isFinite(seconds)) return null;
  const days = Math.floor(seconds / 86_400);
  const hours = Math.floor((seconds % 86_400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  return [days ? `${days} д` : "", hours ? `${hours} ч` : "", `${minutes} мин`].filter(Boolean).join(" ");
}

export function formatSize(width: number | undefined, height: number | undefined): string | null {
  return width && height ? `${width} × ${height} px` : null;
}

export function pluralModules(count: number): string {
  const tens = count % 100;
  const ones = count % 10;
  if (tens >= 11 && tens <= 14) return `${count} модулей`;
  if (ones === 1) return `${count} модуль`;
  if (ones >= 2 && ones <= 4) return `${count} модуля`;
  return `${count} модулей`;
}
