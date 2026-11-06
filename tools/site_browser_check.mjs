// Optional browser verification; artifacts and npm dependencies stay untracked.
import assert from "node:assert/strict";
import { createRequire } from "node:module";
import { mkdirSync, writeFileSync } from "node:fs";
import path from "node:path";

const require = createRequire(path.resolve("artifacts/site-browser/package.json"));
const { chromium } = require("playwright");
const base = process.env.SITE_PREVIEW_URL || "http://127.0.0.1:8000";
const executablePath = process.env.SITE_BROWSER_EXECUTABLE;
const browser = await chromium.launch({ executablePath, headless: true });
const output = "artifacts/site-browser";
mkdirSync(output, { recursive: true });
const results = [];
const routes = ["", "chapters/p01-process/", "chapters/p02-syscalls/",
  "chapters/p03-module/", "chapters/p04-environment/", "chapters/01-why-binder/",
  "chapters/02-character-device/", "chapters/03-sync-rpc/", "labs/",
  "chapters/04-mmap/", "reports/m2-mapping-progress/", "reading-path/",
  "source/driver/mini_binder.c/", "site-guide/", "chapters/05-parcel/",
  "reports/m2-parcel-progress/", "labs/parcel-model/", "source/userspace/parcel.c/",
  "chapters/06-transactions/", "labs/transaction-model/", "design/transactions/",
  "reports/m2-transactions-progress/", "chapters/07-nodes/", "labs/node-model/",
  "design/nodes/", "reports/m3-nodes-progress/"];
try {
  for (const viewport of [{ width: 1365, height: 900 }, { width: 390, height: 844 }]) {
    const context = await browser.newContext({ viewport });
    const externalRequests = [];
    await context.route("**/*", route => {
      const url = route.request().url();
      if (new URL(url).origin !== new URL(base).origin) {
        externalRequests.push(url);
        return route.abort();
      }
      return route.continue();
    });
    await context.addInitScript(() => {
      const attach = Element.prototype.attachShadow;
      window.__siteShadowRoots = new WeakMap();
      Element.prototype.attachShadow = function (options) {
        const root = attach.call(this, options);
        window.__siteShadowRoots.set(this, root);
        return root;
      };
    });
    const page = await context.newPage();
    page.setDefaultTimeout(60000);
    const scriptErrors = [];
    page.on("pageerror", error => scriptErrors.push(String(error)));
    for (const route of routes) {
      const response = await page.goto(`${base}/${route}`, { waitUntil: "networkidle" });
      assert.equal(response.status(), 200, route);
      await page.locator(".md-content h1").waitFor();
      const diagrams = ((await response.text()).match(/class="mermaid"/g) || []).length;
      if (diagrams) {
        await page.waitForFunction(expected => {
          const nodes = [...document.querySelectorAll(".mermaid")];
          return nodes.length === expected && nodes.every(node =>
            window.__siteShadowRoots.get(node)?.querySelector("svg")?.getBoundingClientRect().height > 0);
        }, diagrams);
        await page.locator(".mermaid").first().screenshot({
          path: `${output}/${viewport.width}-${route.replaceAll("/", "-")}mermaid.png`,
          animations: "disabled",
        });
      }
      if (route === "site-guide/") {
        await page.waitForFunction(() => document.querySelector(".arithmatex svg"));
      }
      const dimensions = await page.evaluate(() => ({
        width: document.documentElement.clientWidth,
        scroll: document.documentElement.scrollWidth,
      }));
      assert.ok(dimensions.scroll <= dimensions.width + 1, `${route}: horizontal page overflow`);
      results.push({ viewport, route, diagrams, ...dimensions });
      if (["", "chapters/03-sync-rpc/", "chapters/04-mmap/", "chapters/05-parcel/", "chapters/06-transactions/", "chapters/07-nodes/", "labs/"].includes(route)) {
        await page.screenshot({ path: `${output}/${viewport.width}-${route.replaceAll("/", "-") || "index"}.png`, fullPage: false, animations: "disabled" });
      }
    }
    await page.goto(base);
    if (viewport.width < 1220) {
      await page.locator(".md-header__button[for='__drawer']").click();
      await page.locator(".md-nav--primary").waitFor({ state: "visible" });
      assert.ok(await page.locator("#__drawer").isChecked(), "mobile drawer did not open");
      await page.goto(base);
      await page.locator(".md-header__button[for='__search']").click();
    }
    await page.locator(".md-search__input").fill("RPC");
    await page.locator(".md-search-result__link").first().waitFor({ state: "visible" });
    assert.equal(scriptErrors.length, 0, scriptErrors.join("\n"));
    assert.equal(externalRequests.length, 0, `unexpected external resources: ${externalRequests.join(", ")}`);
    await context.close();
  }
  writeFileSync(`${output}/results.json`, JSON.stringify(results, null, 2) + "\n");
  console.log(`PASS: ${results.length} desktop/mobile pages; Mermaid, formula, search, navigation and overflow checks`);
} finally {
  await browser.close();
}
