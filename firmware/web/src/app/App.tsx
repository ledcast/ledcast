import { AppShell, BottomNav, Crossfade, Dnd, Icon, NotificationProvider, Sidebar } from "prime-ui-kit";
import { CaptionsIcon, CpuIcon, MonitorIcon, WifiIcon } from "prime-ui-kit/icons";
import { type ReactNode, useEffect, useRef, useState } from "react";

import { HardwarePage } from "../pages/HardwarePage.tsx";
import { NetworkPage } from "../pages/NetworkPage.tsx";
import { ShowPage } from "../pages/ShowPage.tsx";
import { SystemPage } from "../pages/SystemPage.tsx";
import { hasSession, onLoginRequired } from "./api.ts";
import { LoginDialog, LoginScreen } from "./Login.tsx";
import styles from "./App.module.css";
import { SectionActiveContext, StatusContext, useStatusPolling } from "./status.ts";

type SectionId = "hardware" | "show" | "network" | "system";

const SECTIONS: { id: SectionId; label: string; icon: ReactNode }[] = [
  { id: "hardware", label: "Оборудование", icon: <CpuIcon /> },
  { id: "show", label: "Шоу", icon: <CaptionsIcon /> },
  { id: "network", label: "Сеть", icon: <WifiIcon /> },
  { id: "system", label: "Система", icon: <Icon name="action.settings" /> },
];

const PAGES: Record<SectionId, () => ReactNode> = {
  hardware: () => <HardwarePage />,
  show: () => <ShowPage />,
  network: () => <NetworkPage />,
  system: () => <SystemPage />,
};

function sectionFromHash(): SectionId {
  const id = window.location.hash.slice(1);
  return SECTIONS.some((section) => section.id === id) ? (id as SectionId) : "hardware";
}

function useSection(): SectionId {
  const [section, setSection] = useState(sectionFromHash);
  useEffect(() => {
    const update = () => setSection(sectionFromHash());
    window.addEventListener("hashchange", update);
    return () => window.removeEventListener("hashchange", update);
  }, []);
  return section;
}

type Access = "checking" | "login" | "ready";

/** Вход посреди работы: запрос ждёт, пока пароль не примут, или падает после отмены. */
function useRelogin() {
  const [open, setOpen] = useState(false);
  const pending = useRef<{ resolve: () => void; reject: (error: Error) => void } | null>(null);
  const waiting = useRef<Promise<void> | null>(null);

  useEffect(() => {
    onLoginRequired(() => {
      waiting.current ??= new Promise<void>((resolve, reject) => {
        pending.current = { resolve, reject };
        setOpen(true);
      }).finally(() => {
        waiting.current = null;
        pending.current = null;
      });
      return waiting.current;
    });
  }, []);

  const finish = (error?: Error) => {
    setOpen(false);
    if (error) pending.current?.reject(error);
    else pending.current?.resolve();
  };

  return { open, done: () => finish(), cancel: () => finish(new Error("Вход отменён, изменение не применено")) };
}

export function App() {
  const [access, setAccess] = useState<Access>("checking");
  const section = useSection();
  const status = useStatusPolling(access === "ready");
  const relogin = useRelogin();

  useEffect(() => {
    hasSession().then((authenticated) => setAccess(authenticated ? "ready" : "login"));
  }, []);

  const deviceName = status.status?.name;

  return (
    <NotificationProvider>
      <Crossfade state={access}>
        {access === "login" ? <LoginScreen onDone={() => setAccess("ready")} /> : null}
        {access === "ready" ? (
          <StatusContext.Provider value={status}>
            <Dnd.Root>
              <AppShell.Template
                fillViewport
                scrollResetKey={section}
                nav={
                  <Sidebar.Root>
                    <Sidebar.Header>
                      <Sidebar.Brand href="#hardware" description={deviceName ?? "Контроллер строки"}>
                        <Sidebar.BrandLogo>
                          <MonitorIcon />
                        </Sidebar.BrandLogo>
                        LEDCAST
                      </Sidebar.Brand>
                      <Sidebar.Toggle variant="header" />
                    </Sidebar.Header>
                    <Sidebar.Content>
                      <Sidebar.Group>
                        {SECTIONS.map((item) => (
                          <Sidebar.Item key={item.id} href={`#${item.id}`} current={item.id === section}>
                            <Sidebar.ItemIcon>{item.icon}</Sidebar.ItemIcon>
                            {item.label}
                          </Sidebar.Item>
                        ))}
                      </Sidebar.Group>
                    </Sidebar.Content>
                  </Sidebar.Root>
                }
                footer={
                  <BottomNav.Root floating>
                    {SECTIONS.map((item) => (
                      <BottomNav.Item key={item.id} href={`#${item.id}`} current={item.id === section}>
                        <BottomNav.ItemIcon>{item.icon}</BottomNav.ItemIcon>
                        {item.label}
                      </BottomNav.Item>
                    ))}
                  </BottomNav.Root>
                }
              >
                {SECTIONS.map((item) => (
                  <SectionActiveContext.Provider key={item.id} value={item.id === section}>
                    <div hidden={item.id !== section} className={styles.section}>
                      {PAGES[item.id]()}
                    </div>
                  </SectionActiveContext.Provider>
                ))}
              </AppShell.Template>
            </Dnd.Root>
          </StatusContext.Provider>
        ) : null}
      </Crossfade>
      <LoginDialog open={relogin.open} onDone={relogin.done} onCancel={relogin.cancel} />
    </NotificationProvider>
  );
}
