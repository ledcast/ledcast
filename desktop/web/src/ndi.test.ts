import assert from "node:assert/strict";
import { test } from "node:test";

import { ndiLabel, ndiMachines, splitNdi } from "./ndi.ts";

test("NDI name splits into computer without its domain and source", () => {
  assert.deepEqual(splitNdi("MBPEUGENESURKOV.LAN (Arena - Screen 4)"), {
    machine: "MBPEUGENESURKOV",
    source: "Arena - Screen 4",
  });
  assert.deepEqual(splitNdi("STUDIO (Out (main))"), { machine: "STUDIO", source: "Out (main)" });
  assert.deepEqual(splitNdi("bare"), { machine: "", source: "bare" });
});

test("computer shows only when sources come from several", () => {
  const one = ["MAC.LAN (A)", "MAC.LAN (B)"];
  assert.deepEqual(ndiMachines(one), ["MAC"]);
  assert.equal(ndiLabel(one[0], false), "A");
  assert.equal(ndiLabel("PC (A)", true), "A · PC");
});
