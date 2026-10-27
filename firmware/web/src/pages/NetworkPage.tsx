import {
  Button,
  Card,
  ConfirmDialog,
  Crossfade,
  Banner,
  EmptyPage,
  Icon,
  Input,
  List,
  Modal,
  Page,
  Skeleton,
  StatusDot,
  Tooltip,
  useNotifications,
} from "prime-ui-kit";
import { LockIcon, WifiIcon, WifiLowIcon } from "prime-ui-kit/icons";
import { type FormEvent, useId, useState } from "react";

import { api } from "../app/api.ts";
import { ConnectionBanner } from "../app/ConnectionBanner.tsx";
import { DeviceStatus } from "../app/DeviceStatus.tsx";
import { usePoll, useStatus } from "../app/status.ts";
import { mergeNetworks, type NetworkEntry, type ScannedNetwork, type WifiProfile } from "../model/network.ts";

const STATE_TEXT: Record<string, string> = {
  sta: "В сети",
  ap_sta: "В сети и точка LEDCAST",
  connecting: "Подключение",
  ap: "Только точка LEDCAST",
  down: "Нет сети",
};

type ScanReply = { scanning?: boolean; failed?: boolean; networks?: ScannedNetwork[] };

const wait = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

// Роутер не пустил строку: неверный пароль, отказ в доступе, сорванное рукопожатие WPA.
const AUTH_FAILURES = new Set([2, 15, 202, 204]);

function entryText(entry: NetworkEntry, scanned: boolean, failed: boolean): string {
  if (entry.connected) return "Подключена";
  if (failed) return "Не подключилась · проверьте пароль";
  if (!entry.available) return scanned ? "Сохранена · не найдена" : "Сохранена";
  if (entry.saved) return "Сохранена";
  return entry.secure === false ? "Открытая" : "Защищена";
}

export function NetworkPage() {
  const { status, refresh } = useStatus();
  const { notify } = useNotifications();
  const [profiles, setProfiles] = useState<WifiProfile[] | null>(null);
  const [scan, setScan] = useState<ScannedNetwork[]>([]);
  const [scanned, setScanned] = useState(false);
  const [scanning, setScanning] = useState(false);
  const [joining, setJoining] = useState<NetworkEntry | null>(null);
  const [forgetting, setForgetting] = useState<NetworkEntry | null>(null);
  const [connecting, setConnecting] = useState<string>();

  const loadProfiles = () =>
    api
      .get<WifiProfile[] | { profiles?: WifiProfile[] }>("/wifi/profiles")
      .then((data) => setProfiles(Array.isArray(data) ? data : (data?.profiles ?? [])))
      .catch(() => setProfiles((current) => current ?? []));
  usePoll(() => void loadProfiles());

  const runScan = async () => {
    setScanning(true);
    const started = Date.now();
    try {
      let reply = await api.get<ScanReply | ScannedNetwork[]>("/wifi/scan");
      while (!Array.isArray(reply) && reply.scanning) {
        if (Date.now() - started > 30_000) throw new Error("Поиск сетей не завершился, повторите");
        await wait(600);
        reply = await api.get<ScanReply | ScannedNetwork[]>("/wifi/scan?poll=1");
      }
      if (!Array.isArray(reply) && reply.failed) throw new Error("Контроллер не смог найти сети, повторите поиск");
      setScan(Array.isArray(reply) ? reply : (reply.networks ?? []));
      setScanned(true);
    } catch (error) {
      notify({ tone: "danger", title: "Сети не найдены", description: (error as Error).message });
    } finally {
      setScanning(false);
    }
  };

  const connectSaved = (entry: NetworkEntry) => {
    if (!entry.profileId) return;
    setConnecting(entry.ssid);
    api
      .post(`/wifi/profiles/${encodeURIComponent(entry.profileId)}/connect`)
      .then(() => {
        notify({ tone: "info", title: `Подключение к «${entry.ssid}»` });
        return Promise.all([loadProfiles(), refresh()]);
      })
      .catch((error: Error) => notify({ tone: "danger", title: "Не подключилась", description: error.message }))
      .finally(() => setConnecting(undefined));
  };

  const forget = async () => {
    if (!forgetting?.profileId) return;
    await api.delete(`/wifi/profiles/${encodeURIComponent(forgetting.profileId)}`);
    notify({ tone: "success", title: `Сеть «${forgetting.ssid}» забыта` });
    await loadProfiles();
  };

  const network = status?.network;
  const entries = profiles ? mergeNetworks(scan, profiles, network) : [];
  // Строка пробует сохранённые сети по порядку, первая — та, что не пустила.
  const failedSsid =
    network?.state === "ap" && AUTH_FAILURES.has(network.last_disconnect_reason ?? 0) ? profiles?.[0]?.ssid : undefined;
  const listState = !profiles ? "loading" : entries.length ? "ready" : "empty";

  return (
    <Page.Root>
      <Page.TopBar>
        <Page.Heading>
          <Page.Title>Сеть</Page.Title>
          <Page.Description loading={!status}>
            {network
              ? [
                  STATE_TEXT[network.state ?? ""] ?? network.state,
                  network.ssid,
                  network.hostname ? `${network.hostname}.local` : null,
                  network.ip || network.ap_ip,
                ]
                  .filter(Boolean)
                  .join(" · ")
              : null}
          </Page.Description>
        </Page.Heading>
        <Page.Actions>
          <DeviceStatus />
        </Page.Actions>
      </Page.TopBar>
      <Page.Body>
        <ConnectionBanner />
        <NetworkBanner />
        <Card.Root>
          <Card.Header>
            <Card.Title as="h2">Сети Wi‑Fi</Card.Title>
            <Card.Description>
              {scanned ? `Найдено сетей: ${new Set(scan.map((item) => item.ssid).filter(Boolean)).size}` : "Сохранённые сети"}
            </Card.Description>
            <Card.Actions>
              <Button.Root variant="soft" tone="neutral" loading={scanning} onClick={() => void runScan()}>
                <Button.Icon>
                  <Icon name="action.refresh" />
                </Button.Icon>
                Найти сети
              </Button.Root>
            </Card.Actions>
          </Card.Header>
          <Card.Body>
            <Crossfade state={listState} aria-busy={listState === "loading" || scanning}>
              {listState === "loading" ? (
                <Skeleton lines={3} />
              ) : null}
              {listState === "empty" ? (
                <EmptyPage.Root size="s" aria-labelledby="networks-empty">
                  <EmptyPage.Icon tone="accent">
                    <WifiIcon />
                  </EmptyPage.Icon>
                  <EmptyPage.Title as="h3" id="networks-empty">
                    Сохранённых сетей нет
                  </EmptyPage.Title>
                  <EmptyPage.Description>Найдите сеть площадки и подключите к ней контроллер.</EmptyPage.Description>
                  <EmptyPage.Actions>
                    <Button.Root loading={scanning} onClick={() => void runScan()}>
                      Найти сети
                    </Button.Root>
                  </EmptyPage.Actions>
                </EmptyPage.Root>
              ) : null}
              {listState === "ready" ? (
                <List.Root aria-label="Сети Wi‑Fi">
                  {entries.map((entry) => (
                    <List.Item key={entry.ssid} value={entry.ssid}>
                      <List.ItemIcon>
                        {entry.available && (entry.rssi ?? -100) > -70 ? <WifiIcon /> : <WifiLowIcon />}
                      </List.ItemIcon>
                      <List.ItemText>{entry.ssid}</List.ItemText>
                      <List.ItemDescription>
                        {entry.connected ? <StatusDot tone="success">{entryText(entry, scanned, entry.ssid === failedSsid)}</StatusDot> : null}
                        {[
                          entry.connected ? null : entryText(entry, scanned, entry.ssid === failedSsid),
                          entry.connected ? network?.ip : null,
                          entry.available && entry.rssi !== undefined ? `${entry.rssi} dBm` : null,
                        ]
                          .filter(Boolean)
                          .join(" · ")}
                      </List.ItemDescription>
                      {entry.secure !== false && !entry.saved && entry.available ? (
                        <List.ItemMeta>
                          <LockIcon label="Защищена паролем" />
                        </List.ItemMeta>
                      ) : null}
                      <List.ItemActions>
                        {!entry.connected && entry.available ? (
                          <Button.Root
                            variant="soft"
                            tone="neutral"
                            loading={connecting === entry.ssid}
                            onClick={() => (entry.saved ? connectSaved(entry) : setJoining(entry))}
                          >
                            Подключить
                          </Button.Root>
                        ) : null}
                        {entry.saved ? (
                          <Tooltip.Root>
                            <Tooltip.Trigger>
                              <Button.Root
                                variant="soft"
                                tone="danger"
                                aria-label={`Забыть сеть «${entry.ssid}»`}
                                onClick={() => setForgetting(entry)}
                              >
                                <Button.Icon>
                                  <Icon name="action.delete" />
                                </Button.Icon>
                              </Button.Root>
                            </Tooltip.Trigger>
                            <Tooltip.Content>Забыть сеть</Tooltip.Content>
                          </Tooltip.Root>
                        ) : null}
                      </List.ItemActions>
                    </List.Item>
                  ))}
                </List.Root>
              ) : null}
            </Crossfade>
          </Card.Body>
        </Card.Root>

        <JoinDialog
          network={joining}
          onClose={() => setJoining(null)}
          onJoined={() => {
            setTimeout(() => void Promise.all([loadProfiles(), refresh()]), 1500);
          }}
        />
        <ConfirmDialog.Root
          open={forgetting !== null}
          onOpenChange={(open) => (open ? undefined : setForgetting(null))}
          title={`Забыть сеть «${forgetting?.ssid ?? ""}»?`}
          description="Контроллер перестанет подключаться к ней сам. Чтобы вернуть, понадобится пароль."
          confirmLabel="Забыть"
          onConfirm={forget}
        />
      </Page.Body>
    </Page.Root>
  );
}

/** Контроллер не в сети площадки: что с ним сейчас и как до него достучаться. */
function NetworkBanner() {
  const { status } = useStatus();
  const network = status?.network;
  if (!network || network.state === "sta" || network.state === "ap_sta") return null;
  if (network.state === "connecting") {
    return (
      <Banner.Root tone="info">
        <Banner.Content>
          <Banner.Title>{network.ssid ? `Подключение к «${network.ssid}»` : "Подключение к сети"}</Banner.Title>
          <Banner.Description>Обычно до минуты</Banner.Description>
        </Banner.Content>
      </Banner.Root>
    );
  }
  return (
    <Banner.Root tone="warning">
      <Banner.Content>
        <Banner.Title>Контроллер не в сети площадки</Banner.Title>
        <Banner.Description>Подключите к локальному Wi‑Fi ниже</Banner.Description>
      </Banner.Content>
    </Banner.Root>
  );
}

function JoinDialog({ network, onClose, onJoined }: { network: NetworkEntry | null; onClose: () => void; onJoined: () => void }) {
  const { notify } = useNotifications();
  const formId = useId();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string>();
  const secure = network?.secure !== false;

  const submit = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    if (!network) return;
    const password = String(new FormData(event.currentTarget).get("password") ?? "");
    if (secure && password.length < 8) {
      setError("Пароль Wi‑Fi — не короче 8 символов");
      return;
    }
    setBusy(true);
    setError(undefined);
    api
      .post("/wifi/profiles", { ssid: network.ssid, password, connect: true })
      .then(() => {
        notify({ tone: "success", title: "Сеть сохранена", description: `Контроллер подключается к «${network.ssid}»` });
        onClose();
        onJoined();
      })
      .catch((reason: Error) => setError(reason.message))
      .finally(() => setBusy(false));
  };

  return (
    <Modal.Root open={network !== null} onOpenChange={(open) => (open ? undefined : onClose())}>
      <Modal.Content size="s">
        <Modal.Guard busy={busy} />
        <Modal.Header>
          <Modal.Title>{network?.ssid}</Modal.Title>
          <Modal.Description>{secure ? "Защищённая сеть Wi‑Fi" : "Открытая сеть Wi‑Fi"}</Modal.Description>
        </Modal.Header>
        <Modal.Body>
          <form id={formId} noValidate onSubmit={submit}>
            {secure ? (
              <Input.Root label="Пароль сети" error={error}>
                <Input.Wrapper>
                  <Input.Field name="password" type="password" autoFocus />
                  <Input.RevealButton />
                </Input.Wrapper>
              </Input.Root>
            ) : null}
          </form>
        </Modal.Body>
        <Modal.Footer>
          <Modal.Close>
            <Button.Root variant="soft" tone="neutral">
              Отмена
            </Button.Root>
          </Modal.Close>
          <Modal.Confirm>
            <Button.Root type="submit" form={formId} loading={busy}>
              Подключить
            </Button.Root>
          </Modal.Confirm>
        </Modal.Footer>
      </Modal.Content>
    </Modal.Root>
  );
}
