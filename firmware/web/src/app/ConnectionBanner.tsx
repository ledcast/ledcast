import { Banner } from "prime-ui-kit";
import { useSyncExternalStore } from "react";

import { connection } from "./api.ts";

/** Первый блок каждой страницы: контроллер перестал отвечать — данные на экране устарели. */
export function ConnectionBanner() {
  const online = useSyncExternalStore(connection.subscribe, connection.get);
  if (online) return null;
  return (
    <Banner.Root tone="danger">
      <Banner.Content>
        <Banner.Title>Нет связи с контроллером</Banner.Title>
        <Banner.Description>Показаны последние данные</Banner.Description>
      </Banner.Content>
    </Banner.Root>
  );
}
