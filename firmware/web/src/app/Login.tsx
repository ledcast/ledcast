import { Button, Icon, Input, LoginForm, Modal } from "prime-ui-kit";
import { type FormEvent, useId, useState } from "react";

import { login } from "./api.ts";
import styles from "./Login.module.css";

function usePasswordForm(onDone: () => void) {
  const [error, setError] = useState<string>();
  const [busy, setBusy] = useState(false);

  const submit = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    const password = String(new FormData(event.currentTarget).get("password") ?? "");
    if (!password) {
      setError("Введите пароль контроллера");
      return;
    }
    setBusy(true);
    setError(undefined);
    login(password)
      .then(onDone)
      .catch((reason: Error) => setError(reason.message))
      .finally(() => setBusy(false));
  };

  return { error, busy, submit };
}

/** Первый вход: контроллер в локальной сети просит пароль, прежде чем показать настройки. */
export function LoginScreen({ deviceName, onDone }: { deviceName?: string; onDone: () => void }) {
  const form = usePasswordForm(onDone);
  return (
    <main className={styles.page}>
      <LoginForm.Root>
        <LoginForm.Header>
          <LoginForm.Logo>
            <Icon name="action.login" />
          </LoginForm.Logo>
          <LoginForm.Title>Вход в контроллер</LoginForm.Title>
          <LoginForm.Description>{deviceName ?? "LEDCAST"} · настройки защищены паролем</LoginForm.Description>
        </LoginForm.Header>
        <LoginForm.Body>
          <LoginForm.Form noValidate onSubmit={form.submit}>
            <Input.Root label="Пароль" error={form.error}>
              <Input.Wrapper>
                <Input.Field name="password" type="password" autoComplete="current-password" autoFocus />
                <Input.RevealButton />
              </Input.Wrapper>
            </Input.Root>
            <Button.Root type="submit" fullWidth loading={form.busy}>
              Войти
            </Button.Root>
          </LoginForm.Form>
        </LoginForm.Body>
      </LoginForm.Root>
    </main>
  );
}

/** Доступ истёк посреди работы: пароль спрашивается поверх экрана, правки не теряются. */
export function LoginDialog({ open, onDone, onCancel }: { open: boolean; onDone: () => void; onCancel: () => void }) {
  const formId = useId();
  const form = usePasswordForm(onDone);
  return (
    <Modal.Root open={open} onOpenChange={(next) => (next ? undefined : onCancel())}>
      <Modal.Content size="s">
        <Modal.Guard busy={form.busy} />
        <Modal.Header>
          <Modal.Icon tone="accent">
            <Icon name="object.key" />
          </Modal.Icon>
          <Modal.Title>Пароль контроллера</Modal.Title>
          <Modal.Description>Доступ истёк. Войдите, и изменение применится.</Modal.Description>
        </Modal.Header>
        <Modal.Body>
          <form id={formId} noValidate onSubmit={form.submit}>
            <Input.Root label="Пароль" error={form.error}>
              <Input.Wrapper>
                <Input.Field name="password" type="password" autoComplete="current-password" autoFocus />
                <Input.RevealButton />
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
            <Button.Root type="submit" form={formId} loading={form.busy}>
              Войти
            </Button.Root>
          </Modal.Confirm>
        </Modal.Footer>
      </Modal.Content>
    </Modal.Root>
  );
}
