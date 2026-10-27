import assert from "node:assert/strict";
import { test } from "node:test";

import { consoleKey } from "./mock-device.ts";

// Тот же вектор, что в firmware/test/test_console_auth.cpp и desktop/src/pairing.rs.
test("ключ Console совпадает с прошивкой и Console", () => {
  assert.equal(consoleKey("ledcast1", "ledcast-7f2a91").toString("hex"), "4e865ed217587b470a82e5c53c27188963cc2a4afd78afaac157a8935a544fb2");
  assert.equal(consoleKey("88888888", "A0B1C2D3E4F5").toString("hex"), "35ca6fc7a96b71b89b46fa3d87f2bf81cc1e4e01db4e72df406118be54c887e4");
});
