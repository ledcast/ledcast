import {
  Button,
  Card,
  ConfirmDialog,
  DescriptionList,
  Fieldset,
  FileUpload,
  Icon,
  Input,
  Modal,
  Page,
  useNotifications,
} from "prime-ui-kit";
import { type FormEvent, useEffect, useId, useState } from "react";

import { api, uploadFirmware } from "../app/api.ts";
import { ConnectionBanner } from "../app/ConnectionBanner.tsx";
import { DeviceStatus } from "../app/DeviceStatus.tsx";
import { formatBytes, formatDuration } from "../app/format.ts";
import { useStatus } from "../app/status.ts";
import styles from "./SystemPage.module.css";

const RESET_WORD = "СБРОС";

export function SystemPage() {
  const { status } = useStatus();

  return (
    <Page.Root>
      <Page.TopBar>
        <Page.Heading>
          <Page.Title>Система</Page.Title>
        </Page.Heading>
        <Page.Actions>
          <DeviceStatus />
        </Page.Actions>
      </Page.TopBar>
      <Page.Body>
        <ConnectionBanner />
        <div className={styles.columns}>
          <div className={styles.main}>
            <DeviceCard />
            <MaintenanceCard />
          </div>
          <div className={styles.side}>
            <Card.Root>
              <Card.Header>
                <Card.Title as="h2">Контроллер</Card.Title>
              </Card.Header>
              <Card.Body>
                <DescriptionList.Root align="end">
                  <DescriptionList.Item label="Прошивка">{status?.firmware}</DescriptionList.Item>
                  <DescriptionList.Item label="Время работы">{formatDuration(status?.uptime_s)}</DescriptionList.Item>
                  <DescriptionList.Item label="Свободная память">{formatBytes(status?.memory?.free_heap)}</DescriptionList.Item>
                </DescriptionList.Root>
              </Card.Body>
            </Card.Root>
          </div>
        </div>
      </Page.Body>
    </Page.Root>
  );
}

function DeviceCard() {
  const { status, refresh } = useStatus();
  const [saving, setSaving] = useState(false);
  const [saved, setSaved] = useState(false);
  const [passwordError, setPasswordError] = useState<string>();
  const [formKey, setFormKey] = useState(0);
  const { notify } = useNotifications();

  useEffect(() => {
    if (!saved) return;
    const timer = setTimeout(() => setSaved(false), 2000);
    return () => clearTimeout(timer);
  }, [saved]);

  const save = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const form = new FormData(event.currentTarget);
    const name = String(form.get("name") ?? "").trim();
    const password = String(form.get("password") ?? "");
    if (password && password.length < 8) {
      setPasswordError("Не короче 8 символов");
      return;
    }
    setPasswordError(undefined);
    setSaving(true);
    api
      .put("/system", { name, password })
      .then(() => {
        setSaved(true);
        setFormKey((key) => key + 1);
        return refresh();
      })
      .catch((error: Error) => notify({ tone: "danger", title: "Настройки не сохранены", description: error.message }))
      .finally(() => setSaving(false));
  };

  return (
    <form noValidate onSubmit={save} onReset={() => setPasswordError(undefined)} key={`${formKey}-${status?.name ?? ""}`}>
      <Card.Root>
        <Card.Header>
          <Card.Title as="h2" number={status?.id}>
            Устройство
          </Card.Title>
          <Card.Description>Имя видно в LEDCAST Console и в сети</Card.Description>
        </Card.Header>
        <Card.Body>
          <Fieldset disabled={saving} layout="pair">
            <Input.Root label="Имя строки">
              <Input.Wrapper>
                <Input.Field name="name" maxLength={31} placeholder="LEDCAST LINE 01" defaultValue={status?.name ?? ""} />
              </Input.Wrapper>
            </Input.Root>
            <Input.Root label="Новый пароль" optional hint="Пусто — пароль не меняется" error={passwordError} reserveSupportRow>
              <Input.Wrapper>
                <Input.Field name="password" type="password" minLength={8} maxLength={63} autoComplete="new-password" />
                <Input.RevealButton />
              </Input.Wrapper>
            </Input.Root>
          </Fieldset>
        </Card.Body>
        <Card.Footer>
          <Card.Actions>
            <Button.Root type="reset" variant="soft" tone="neutral" disabled={saving}>
              Отменить изменения
            </Button.Root>
            <Button.Root type="submit" loading={saving}>
              {saved ? "Сохранено" : "Сохранить"}
            </Button.Root>
          </Card.Actions>
        </Card.Footer>
      </Card.Root>
    </form>
  );
}

function MaintenanceCard() {
  const { notify } = useNotifications();
  const { refresh } = useStatus();
  const [file, setFile] = useState<File | null>(null);
  const [progress, setProgress] = useState<number | null>(null);
  const [resetOpen, setResetOpen] = useState(false);

  const install = () => {
    if (!file) return;
    setProgress(0);
    uploadFirmware(file, setProgress)
      .then(() => {
        notify({ tone: "success", title: "Прошивка загружена", description: "Контроллер перезагружается и вернётся в сеть сам" });
        setFile(null);
      })
      .catch((error: Error) => notify({ tone: "danger", title: "Прошивка не установлена", description: error.message }))
      .finally(() => setProgress(null));
  };

  const reboot = async () => {
    await api.post("/reboot");
    notify({ tone: "info", title: "Контроллер перезагружается" });
    setTimeout(() => void refresh(), 4000);
  };

  return (
    <>
      <Card.Root>
        <Card.Header>
          <Card.Title as="h2">Обновление прошивки</Card.Title>
          <Card.Description>Контроллер перезагрузится с новой прошивкой, настройки сохранятся</Card.Description>
        </Card.Header>
        <Card.Body>
          <FileUpload.Root
            aria-label="Файл прошивки"
            hint="Файл ledcast-node.bin из релиза LEDCAST"
            trigger="button"
            accept=".bin,application/octet-stream"
            max={1}
            disabled={progress !== null}
            value={file ? [{ id: "firmware", name: file.name, size: file.size }] : []}
            onFilesChange={(files) => setFile(files[0] ?? null)}
            onValueChange={(value) => (value.length ? undefined : setFile(null))}
          >
            <Button.Icon>
              <Icon name="action.attach" />
            </Button.Icon>
            Выбрать файл
          </FileUpload.Root>
        </Card.Body>
        <Card.Footer>
          <Card.Actions>
            <ConfirmDialog.Root
              tone="accent"
              title={`Установить «${file?.name ?? ""}»?`}
              description="Контроллер перезагрузится с новой прошивкой. Настройки сохранятся."
              confirmLabel="Установить"
              onConfirm={install}
            >
              <ConfirmDialog.Trigger>
                <Button.Root disabled={!file} progress={progress ?? undefined}>
                  <Button.Icon>
                    <Icon name="action.upload" />
                  </Button.Icon>
                  {progress === null ? "Установить прошивку" : `Загрузка ${Math.round(progress * 100)}%`}
                </Button.Root>
              </ConfirmDialog.Trigger>
            </ConfirmDialog.Root>
          </Card.Actions>
        </Card.Footer>
      </Card.Root>

      <Card.Root>
        <Card.Header>
          <Card.Title as="h2">Обслуживание</Card.Title>
          <Card.Description>Изображение пропадёт, пока контроллер перезапускается</Card.Description>
          <Card.Actions>
            <Button.Root variant="soft" tone="danger" onClick={() => setResetOpen(true)}>
              <Button.Icon>
                <Icon name="action.delete" />
              </Button.Icon>
              Заводской сброс
            </Button.Root>
            <ConfirmDialog.Root
              tone="accent"
              title="Перезагрузить контроллер?"
              description="Изображение пропадёт на несколько секунд, контроллер вернётся в сеть сам."
              confirmLabel="Перезагрузить"
              onConfirm={reboot}
            >
              <ConfirmDialog.Trigger>
                <Button.Root variant="soft" tone="neutral">
                  <Button.Icon>
                    <Icon name="action.power" />
                  </Button.Icon>
                  Перезагрузить
                </Button.Root>
              </ConfirmDialog.Trigger>
            </ConfirmDialog.Root>
          </Card.Actions>
        </Card.Header>
        <FactoryResetDialog open={resetOpen} onOpenChange={setResetOpen} />
      </Card.Root>
    </>
  );
}

/** Сброс стирает и сеть: подтверждается словом, а не одним нажатием. */
function FactoryResetDialog({ open, onOpenChange }: { open: boolean; onOpenChange: (open: boolean) => void }) {
  const { notify } = useNotifications();
  const formId = useId();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string>();

  const submit = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    if (String(new FormData(event.currentTarget).get("word") ?? "").trim().toUpperCase() !== RESET_WORD) {
      setError(`Введите ${RESET_WORD}`);
      return;
    }
    setBusy(true);
    setError(undefined);
    api
      .post("/factory-reset")
      .then(() => {
        onOpenChange(false);
        notify({ tone: "success", title: "Настройки стёрты", description: "Подключитесь к точке Wi‑Fi LEDCAST, чтобы настроить контроллер заново", persistent: true });
      })
      .catch((reason: Error) => setError(reason.message))
      .finally(() => setBusy(false));
  };

  return (
    <Modal.Root open={open} onOpenChange={onOpenChange}>
      <Modal.Content size="s">
        <Modal.Guard busy={busy} />
        <Modal.Header>
          <Modal.Icon tone="danger">
            <Icon name="status.warning" />
          </Modal.Icon>
          <Modal.Title>Стереть все настройки?</Modal.Title>
          <Modal.Description>Удалятся оборудование, сети Wi‑Fi и пароль. Контроллер откроет точку LEDCAST.</Modal.Description>
        </Modal.Header>
        <Modal.Body>
          <form id={formId} noValidate onSubmit={submit}>
            <Input.Root label={`Для подтверждения введите ${RESET_WORD}`} error={error}>
              <Input.Wrapper>
                <Input.Field name="word" autoComplete="off" placeholder={RESET_WORD} />
              </Input.Wrapper>
            </Input.Root>
          </form>
        </Modal.Body>
        <Modal.Footer>
          <Modal.Close>
            <Button.Root variant="soft" tone="neutral">
              Отмена
            </Button.Root>
          </Modal.Close>
          <Modal.Confirm>
            <Button.Root type="submit" form={formId} tone="danger" loading={busy}>
              Стереть настройки
            </Button.Root>
          </Modal.Confirm>
        </Modal.Footer>
      </Modal.Content>
    </Modal.Root>
  );
}
