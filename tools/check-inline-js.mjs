#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
//
// Syntax-check the inline <script> blocks of HTML files with `node --check`.
//
//   node tools/check-inline-js.mjs main/web/index.html web-installer/index.html
//
// Scripts with a src= attribute or a non-JavaScript type (JSON, templates) are skipped.
// type="module" scripts are checked as ES modules. Exits non-zero on the first file
// that is missing or on any syntax error.
import { readFileSync, writeFileSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";

const files = process.argv.slice(2);
if (files.length === 0) {
  console.error("usage: check-inline-js.mjs <file.html>...");
  process.exit(2);
}

const JS_TYPES = new Set(["", "text/javascript", "application/javascript", "module"]);
const dir = mkdtempSync(join(tmpdir(), "inline-js-"));
let failed = 0;
let checked = 0;

try {
  for (const file of files) {
    let html;
    try {
      html = readFileSync(file, "utf8");
    } catch (e) {
      console.error(`::error file=${file}::cannot read ${file}: ${e.message}`);
      failed++;
      continue;
    }
    const re = /<script\b([^>]*)>([\s\S]*?)<\/script\s*>/gi;
    let m;
    let n = 0;
    while ((m = re.exec(html)) !== null) {
      const attrs = m[1];
      const body = m[2];
      if (/\bsrc\s*=/i.test(attrs)) continue;
      const type = (/\btype\s*=\s*["']?([^"'\s>]+)/i.exec(attrs)?.[1] ?? "").toLowerCase();
      if (!JS_TYPES.has(type)) continue;
      n++;
      // Line of the <script> tag, so errors can be mapped back to the HTML file.
      const line = html.slice(0, m.index).split("\n").length;
      const tmp = join(dir, `script${checked++}.${type === "module" ? "mjs" : "js"}`);
      writeFileSync(tmp, body);
      const r = spawnSync(process.execPath, ["--check", tmp], { encoding: "utf8" });
      if (r.status !== 0) {
        failed++;
        const msg = (r.stderr || r.stdout).replaceAll(tmp, `${file} <script #${n}, tag at line ${line}>`);
        console.error(`::error file=${file},line=${line}::inline script #${n} has a syntax error`);
        console.error(msg);
      }
    }
    console.log(`${file}: ${n} inline script(s) checked`);
  }
} finally {
  rmSync(dir, { recursive: true, force: true });
}

process.exit(failed ? 1 : 0);
