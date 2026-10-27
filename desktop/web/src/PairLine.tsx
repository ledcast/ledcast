import { Button, Fieldset, Icon, Input, Modal, useNotifications } from "prime-ui-kit";
import { type FormEvent, useId, useState } from "react";

import type { ConsoleStore } from "./store.ts";
import type { LineNode } from "./tauri.ts";

/** Ошибка команды Tauri приходит строкой, ошибка JS — объектом. */
function reason(error: unknown): string {
  if (typeof error === "string") return error;
  if (error instanceof Error) return error.message;
  return String(error);
}

type PairLineProps = { node: LineNode; store: ConsoleStore };

/** «Подключить»: пароль строки один раз, дальше Console входит своим ключом. */
export function PairLine({ node, store }: PairLineProps) {
  const { notify } = useNotifications();
  const formId = useId();
  const [open, setOpen] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string>();
  const title = node.name || node.host;

  const submit = async (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const password = String(new FormData(event.currentTarget).get("password") ?? "");
    if (!password) {
      setError("Введите пароль строки");
      return;
    }
    setBusy(true);
    try {
      await store.pairLine(node, password);
      setOpen(false);
      notify({ tone: "success", title: `«${title}» подключена` });
    } catch (failure) {
      setError(reason(failure));
    } finally {
      setBusy(false);
    }
  };

  return (
    <Modal.Root
      open={open}
      onOpenChange={(next) => {
        setOpen(next);
        if (!next) setError(undefined);
      }}
    >
      <Modal.Trigger>
        <Button.Root variant="soft" tone="neutral">
          <Button.Icon>
            <Icon name="object.key" />
          </Button.Icon>
          Подключить
        </Button.Root>
      </Modal.Trigger>
      <Modal.Content size="s">
        <Modal.Guard busy={busy} />
        <Modal.Header>
          <Modal.Title>{`Подключить «${title}»`}</Modal.Title>
        </Modal.Header>
        <Modal.Body>
          <form id={formId} noValidate onSubmit={(event) => void submit(event)}>
            <Fieldset disabled={busy}>
              <Input.Root label="Пароль" error={error}>
                <Input.Wrapper>
                  <Input.Field name="password" type="password" autoFocus onValueChange={() => setError(undefined)} />
                  <Input.RevealButton />
                </Input.Wrapper>
              </Input.Root>
            </Fieldset>
          </form>
        </Modal.Body>
        <Modal.Footer>
          <Modal.Close>
            <Button.Root variant="soft" tone="neutral">
              Отмена
            </Button.Root>
          </Modal.Close>
          <Button.Root type="submit" form={formId} loading={busy}>
            Подключить
          </Button.Root>
        </Modal.Footer>
      </Modal.Content>
    </Modal.Root>
  );
}
