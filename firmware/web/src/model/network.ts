// Список сетей Wi‑Fi: результаты сканирования, сохранённые профили и текущее подключение
// сводятся в один список без повторов.

export type ScannedNetwork = { ssid: string; rssi?: number; secure?: boolean };
export type WifiProfile = { id?: string; ssid: string };
export type NetworkStatus = {
  state?: string;
  ssid?: string;
  rssi?: number;
  ip?: string;
  ap_ssid?: string;
  ap_ip?: string;
  /** Имя строки в сети: отвечает на <hostname>.local и у роутера, и на своей точке доступа. */
  hostname?: string;
  /** Причина последнего разрыва Wi‑Fi (коды ESP-IDF wifi_err_reason_t). */
  last_disconnect_reason?: number;
};

export type NetworkEntry = {
  ssid: string;
  rssi?: number;
  secure?: boolean;
  saved: boolean;
  profileId?: string;
  available: boolean;
  connected: boolean;
};

export function connectedSsid(network: NetworkStatus | undefined): string | null {
  return network && (network.state === "sta" || network.state === "ap_sta") ? (network.ssid ?? null) : null;
}

function rank(entry: NetworkEntry): number {
  if (entry.connected) return 0;
  if (!entry.available) return 3;
  return entry.saved ? 1 : 2;
}

export function mergeNetworks(
  scanned: readonly ScannedNetwork[],
  profiles: readonly WifiProfile[],
  network: NetworkStatus | undefined,
): NetworkEntry[] {
  const connected = connectedSsid(network);
  const entries = new Map<string, NetworkEntry>();
  for (const net of scanned) {
    if (!net.ssid) continue;
    const previous = entries.get(net.ssid);
    if (!previous || (net.rssi ?? -100) > (previous.rssi ?? -100)) {
      entries.set(net.ssid, { ssid: net.ssid, rssi: net.rssi, secure: net.secure, saved: false, available: true, connected: false });
    }
  }
  for (const profile of profiles) {
    if (!profile.ssid) continue;
    const entry = entries.get(profile.ssid) ?? { ssid: profile.ssid, saved: false, available: false, connected: false };
    entries.set(profile.ssid, { ...entry, saved: true, profileId: profile.id || profile.ssid });
  }
  if (connected && !entries.has(connected)) {
    entries.set(connected, { ssid: connected, saved: false, available: true, connected: false });
  }
  return Array.from(entries.values())
    .map((entry) =>
      entry.ssid === connected ? { ...entry, connected: true, available: true, rssi: network?.rssi } : entry,
    )
    .sort(
      (a, b) => rank(a) - rank(b) || (b.rssi ?? -100) - (a.rssi ?? -100) || a.ssid.localeCompare(b.ssid),
    );
}
