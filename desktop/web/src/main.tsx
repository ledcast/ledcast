import "prime-ui-kit/reset.css";
import "@fontsource/golos-text/cyrillic-400.css";
import "@fontsource/golos-text/cyrillic-500.css";
import "@fontsource/golos-text/cyrillic-600.css";
import "@fontsource/golos-text/latin-400.css";
import "@fontsource/golos-text/latin-500.css";
import "@fontsource/golos-text/latin-600.css";

import { applyTheme, NotificationProvider } from "prime-ui-kit";
import { createRoot } from "react-dom/client";

import { App } from "./App.tsx";

applyTheme("dark");

async function start() {
  // Вне Tauri (npm run dev в браузере) Console отвечает мок со строками-примерами.
  if (import.meta.env.DEV && !window.__TAURI__) {
    const { installMockConsole } = await import("./dev/mockConsole.ts");
    installMockConsole();
  }
  const root = document.getElementById("root");
  if (root) {
    createRoot(root).render(
      <NotificationProvider>
        <App />
      </NotificationProvider>,
    );
  }
}

void start();
