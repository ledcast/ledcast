// Запросы к контроллеру. Изменения в локальной сети требуют пароль: ответ 401 открывает
// вход, после входа запрос повторяется один раз.

const API = "/api/v1";

export class HttpError extends Error {
  readonly status: number;
  constructor(message: string, status: number) {
    super(message);
    this.status = status;
  }
}

declare global {
  interface Window {
    /** Сессия, которую LEDCAST Console выдаёт окну контроллера: вход без пароля. */
    __LEDCAST_SESSION__?: unknown;
  }
}

const consoleSession = typeof window === "undefined" ? undefined : window.__LEDCAST_SESSION__;
let token: string | null = typeof consoleSession === "string" && consoleSession ? consoleSession : null;
let requestLogin: (() => Promise<void>) | null = null;
let online = true;
const listeners = new Set<() => void>();

function setOnline(value: boolean) {
  if (online === value) return;
  online = value;
  for (const listener of listeners) listener();
}

/** Связь с контроллером: для useSyncExternalStore. */
export const connection = {
  subscribe(listener: () => void) {
    listeners.add(listener);
    return () => listeners.delete(listener);
  },
  get: () => online,
};

/** Приложение показывает вход и разрешает промис, когда пароль принят. */
export function onLoginRequired(handler: () => Promise<void>) {
  requestLogin = handler;
}

function errorMessage(data: unknown, raw: string, status: number): string {
  if (data && typeof data === "object" && "error" in data && typeof data.error === "string") return data.error;
  return raw || `HTTP ${status}`;
}

async function send<T>(method: string, path: string, body: unknown, retry: boolean): Promise<T> {
  const headers: Record<string, string> = { Accept: "application/json" };
  if (token) headers.Authorization = `Bearer ${token}`;
  if (body !== undefined) headers["Content-Type"] = "application/json";
  let response: Response;
  try {
    response = await fetch(API + path, { method, headers, body: body === undefined ? undefined : JSON.stringify(body) });
  } catch {
    setOnline(false);
    throw new Error("Нет связи с контроллером");
  }
  setOnline(true);
  const raw = await response.text();
  let data: unknown = null;
  try {
    data = raw ? JSON.parse(raw) : null;
  } catch {
    // Тело не JSON: текст ошибки берётся как есть.
  }
  if (response.status === 401 && retry && requestLogin) {
    await requestLogin();
    return send(method, path, body, false);
  }
  if (!response.ok) throw new HttpError(errorMessage(data, raw, response.status), response.status);
  return data as T;
}

export const api = {
  get: <T>(path: string) => send<T>("GET", path, undefined, true),
  post: <T>(path: string, body: unknown = {}) => send<T>("POST", path, body, true),
  put: <T>(path: string, body: unknown) => send<T>("PUT", path, body, true),
  delete: <T>(path: string) => send<T>("DELETE", path, undefined, true),
};

export async function hasSession(): Promise<boolean> {
  try {
    const headers: Record<string, string> = { Accept: "application/json" };
    if (token) headers.Authorization = `Bearer ${token}`;
    const response = await fetch(`${API}/auth/session`, { headers });
    const data = (await response.json()) as { authenticated?: boolean };
    return Boolean(data.authenticated);
  } catch {
    return false;
  }
}

export async function login(password: string): Promise<void> {
  const response = await fetch(`${API}/auth/login`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ password }),
  });
  const data = (await response.json().catch(() => ({}))) as { error?: string; token?: string; access_token?: string };
  if (!response.ok) throw new Error(data.error || "Неверный пароль");
  token = data.token || data.access_token || null;
  if (!token) throw new Error("Контроллер не выдал доступ, повторите вход");
}

/** Прошивка уходит телом запроса; прогресс — доля от 0 до 1. */
export function uploadFirmware(file: File, onProgress: (share: number) => void, retry = true): Promise<void> {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open("POST", `${API}/ota`);
    if (token) xhr.setRequestHeader("Authorization", `Bearer ${token}`);
    xhr.setRequestHeader("Content-Type", "application/octet-stream");
    xhr.upload.onprogress = (event) => {
      if (event.lengthComputable) onProgress(event.loaded / event.total);
    };
    xhr.onload = () => {
      if (xhr.status === 401 && retry && requestLogin) {
        requestLogin()
          .then(() => uploadFirmware(file, onProgress, false))
          .then(resolve, reject);
      } else if (xhr.status >= 200 && xhr.status < 300) {
        resolve();
      } else {
        reject(new HttpError(xhr.responseText || `HTTP ${xhr.status}`, xhr.status));
      }
    };
    xhr.onerror = () => reject(new Error("Соединение прервалось во время загрузки"));
    xhr.send(file);
  });
}
