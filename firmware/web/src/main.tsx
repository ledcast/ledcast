import "prime-ui-kit/reset.css";

import { applyTheme } from "prime-ui-kit";
import { createRoot } from "react-dom/client";

import { App } from "./app/App.tsx";

// Строку настраивают у сцены, в темноте: тема всегда тёмная.
applyTheme("dark");

const root = document.getElementById("root");
if (root) createRoot(root).render(<App />);
