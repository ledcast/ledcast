use std::path::Path;
use std::process::Command;

fn main() {
    build_web();
    tauri_build::build();
}

/// Интерфейс Console (React + prime-ui-kit) собирается Vite в web/dist, а Tauri встраивает
/// его в бинарник: без свежей сборки приложение показало бы старую страницу.
fn build_web() {
    let web = Path::new("web");
    for path in ["src", "index.html", "package.json", "vite.config.ts"] {
        println!("cargo:rerun-if-changed={}", web.join(path).display());
    }
    if !Path::new("../node_modules").is_dir() {
        panic!("Нет зависимостей интерфейса Console: выполните `npm ci` в корне репозитория");
    }
    let npm = if cfg!(windows) { "npm.cmd" } else { "npm" };
    let status = Command::new(npm)
        .args(["run", "build"])
        .current_dir(web)
        .status()
        .expect("запустить npm: нужен Node.js (https://nodejs.org)");
    assert!(status.success(), "сборка интерфейса Console не прошла");
}
