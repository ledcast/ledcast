// Контроллер для разработки без железа: `npm run dev` без LEDCAST_DEVICE отвечает на
// /api/v1 из памяти. С LEDCAST_DEVICE=http://<ip> запросы уходят на настоящее устройство.

import { createHmac, randomBytes, timingSafeEqual } from "node:crypto";
import type { IncomingMessage, ServerResponse } from "node:http";
import type { Plugin } from "vite";

const started = Date.now();
const DEVICE_ID = "ledcast-7f2a91";

// Вход как на устройстве: сессии на 12 ч, одноразовые nonce на 30 с для Console.
const sessions = new Map<string, number>();
const nonces = new Map<string, number>();

/** Ключ Console: HMAC-SHA256(пароль, "ledcast-console:" + id) — как в прошивке и в Console. */
export function consoleKey(password: string, id: string): Buffer {
  return createHmac("sha256", password).update(`ledcast-console:${id}`).digest();
}

function openSession(): string {
  const token = randomBytes(32).toString("hex");
  sessions.set(token, Date.now() + 12 * 3600_000);
  return token;
}

function authorized(req: IncomingMessage): boolean {
  const header = req.headers.authorization ?? "";
  const token = header.startsWith("Bearer ") ? header.slice(7) : "";
  return (sessions.get(token) ?? 0) > Date.now();
}

const state = {
  name: "LEDCAST LINE 01",
  password: "ledcast1",
  testPattern: "",
  hardware: {
    module: {
      width: 32,
      height: 16,
      scan_divisor: 4,
      layout: "spiral",
      color_order: "RGB",
      shift_driver: "generic",
      line_decoder: "138",
      oe_active_low: true,
      data_active_high: true,
    },
    module_count: 6,
    modules: [6, 5, 4, 3, 2, 1].map((physical) => ({ physical, rotate_180: physical === 3, mirror_x: false, mirror_y: false })),
    display: { brightness: 160, color_depth: 6, dma_buffers: 2, bus_hz: 20_000_000, clk_phase: true, latch_blanking: 1, frame_rate: 60 },
  },
  show: {
    enabled: false,
    mode: "scroll",
    font_size_px: 12,
    speed_px_s: 48,
    duration_s: 0,
    color: "#FF8000",
    text: "ПОПКОРН\nТАТУ-ЗОНА\nБАР ОТКРЫТ ДО 04:00",
  },
  profiles: [
    { id: "p1", ssid: "Venue-Stage" },
    { id: "p2", ssid: "Office 5G" },
  ],
  network: { state: "sta", ssid: "Venue-Stage", ip: "192.168.1.47", rssi: -54, ap_ssid: "LEDCAST-7F2A", ap_ip: "", hostname: "ledcast-7f2a" },
  scanUntil: 0,
};

const SCAN = [
  { ssid: "Venue-Stage", rssi: -52, secure: true },
  { ssid: "Venue-Guest", rssi: -61, secure: false },
  { ssid: "Bar_WiFi", rssi: -70, secure: true },
  { ssid: "DIRECT-printer", rssi: -83, secure: true },
];

function status() {
  const { module, module_count, display } = state.hardware;
  const uptime = Math.floor((Date.now() - started) / 1000) + 3 * 86_400 + 5 * 3600;
  return {
    product: "LEDCAST",
    id: DEVICE_ID,
    name: state.name,
    firmware: "0.1.0",
    test_pattern: state.testPattern,
    show_enabled: state.show.enabled,
    show_control: true,
    modules: module_count,
    width: module.width * module_count,
    height: module.height,
    uptime_s: uptime,
    memory: { free_heap: 142_336 + Math.round(Math.random() * 4096) },
    network: state.network,
    display: { ready: true, brightness: display.brightness, frame_rate: display.frame_rate, fps: display.frame_rate, refresh_hz: 1920, dma_bytes: 98_304 },
    stream: { signal: !state.show.enabled, fps: state.show.enabled ? 0 : 50, received: 1_284_311 + uptime * 50, dropped: 37, source_ip: "192.168.1.20", clock_synced: true, reservoir_frames: 18 },
  };
}

function maxFont() {
  return Math.min(14, state.hardware.module.height);
}

async function body(req: IncomingMessage): Promise<Record<string, unknown>> {
  const chunks: Buffer[] = [];
  for await (const chunk of req) chunks.push(chunk as Buffer);
  const raw = Buffer.concat(chunks).toString("utf8");
  if (!raw || req.headers["content-type"] === "application/octet-stream") return {};
  return JSON.parse(raw) as Record<string, unknown>;
}

function send(res: ServerResponse, code: number, data: unknown) {
  res.statusCode = code;
  res.setHeader("Content-Type", "application/json");
  res.end(JSON.stringify(data ?? {}));
}

async function handle(req: IncomingMessage, res: ServerResponse) {
  const url = new URL(req.url ?? "/", "http://device");
  const route = `${req.method} ${url.pathname.replace("/api/v1", "")}`;
  const delay = route.startsWith("GET") ? 120 : 350;
  await new Promise((resolve) => setTimeout(resolve, delay));

  if (route === "GET /auth/session") return send(res, 200, { authenticated: authorized(req) });
  if (route === "POST /auth/login") {
    const { password } = await body(req);
    return password === state.password ? send(res, 200, { token: openSession(), expires_in: 43200 }) : send(res, 401, { error: "Неверный пароль" });
  }
  if (route === "GET /auth/challenge") {
    const nonce = randomBytes(32).toString("hex");
    nonces.set(nonce, Date.now() + 30_000);
    return send(res, 200, { nonce, expires_in: 30 });
  }
  if (route === "POST /auth/console") {
    const { nonce, proof } = await body(req);
    const expiry = typeof nonce === "string" ? nonces.get(nonce) : undefined;
    if (typeof nonce === "string") nonces.delete(nonce);
    const expected = Buffer.from(createHmac("sha256", consoleKey(state.password, DEVICE_ID)).update(String(nonce)).digest("hex"));
    const given = Buffer.from(String(proof ?? ""));
    const ok = expiry !== undefined && expiry > Date.now() && given.length === expected.length && timingSafeEqual(given, expected);
    return ok ? send(res, 200, { token: openSession(), expires_in: 43200 }) : send(res, 401, { error: "Требуется пароль устройства" });
  }
  if ((route === "PUT /show/enabled" || route === "PUT /brightness") && !authorized(req)) {
    return send(res, 401, { error: "Требуется пароль устройства" });
  }
  if (route === "GET /status") return send(res, 200, status());
  if (route === "GET /hardware") return send(res, 200, state.hardware);
  if (route === "PUT /hardware") {
    state.hardware = (await body(req)) as typeof state.hardware;
    return send(res, 200, state.hardware);
  }
  if (route === "GET /show") return send(res, 200, { ...state.show, font_size_px: Math.min(state.show.font_size_px, maxFont()), max_font_size_px: maxFont() });
  if (route === "PUT /show") {
    Object.assign(state.show, await body(req));
    return send(res, 200, { ...state.show, max_font_size_px: maxFont() });
  }
  if (route === "PUT /show/enabled") {
    state.show.enabled = Boolean((await body(req)).enabled);
    return send(res, 200, { enabled: state.show.enabled });
  }
  if (route === "PUT /brightness") {
    state.hardware.display.brightness = Number((await body(req)).value);
    return send(res, 200, { value: state.hardware.display.brightness });
  }
  if (route === "POST /test") {
    const data = await body(req);
    state.testPattern = data.release ? "" : String(data.pattern ?? "");
    return send(res, 200, { pattern: state.testPattern });
  }
  if (route === "GET /wifi/profiles") return send(res, 200, state.profiles);
  if (route === "GET /wifi/scan") {
    if (!url.searchParams.has("poll")) state.scanUntil = Date.now() + 2500;
    return Date.now() < state.scanUntil ? send(res, 200, { scanning: true }) : send(res, 200, { networks: SCAN });
  }
  if (route === "POST /wifi/profiles") {
    const { ssid } = await body(req);
    state.profiles.push({ id: `p${Date.now()}`, ssid: String(ssid) });
    return send(res, 200, {});
  }
  const profile = url.pathname.match(/\/wifi\/profiles\/([^/]+)(\/connect)?$/);
  if (profile) {
    const id = decodeURIComponent(profile[1]);
    if (req.method === "DELETE") state.profiles = state.profiles.filter((item) => item.id !== id);
    else state.network = { ...state.network, ssid: state.profiles.find((item) => item.id === id)?.ssid ?? state.network.ssid };
    return send(res, 200, {});
  }
  if (route === "PUT /system") {
    const { name } = await body(req);
    state.name = String(name || state.name);
    return send(res, 200, {});
  }
  if (route === "POST /ota") {
    await body(req);
    return send(res, 200, {});
  }
  if (route === "POST /reboot" || route === "POST /factory-reset") return send(res, 200, {});
  return send(res, 404, { error: `Нет такого запроса: ${route}` });
}

export function mockDevice(): Plugin {
  return {
    name: "ledcast-mock-device",
    apply: "serve",
    configureServer(server) {
      server.middlewares.use((req, res, next) => {
        if (!req.url?.startsWith("/api/v1/")) return next();
        handle(req, res).catch((error: Error) => send(res, 500, { error: error.message }));
      });
    },
  };
}
