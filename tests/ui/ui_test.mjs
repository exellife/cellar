// pgforge admin UI headless test: render the real served SPA in jsdom, drive a
// login, and assert the schema-driven dashboard renders against a live server.
import { JSDOM } from "jsdom";

const arg = process.argv[2] || "ws://127.0.0.1:8080/";
const base = arg.replace(/^ws/, "http").replace(/\/$/, "");
const EMAIL = process.env.PGF_TEST_EMAIL || "admin@pgforge.dev";
const PW = process.env.PGF_TEST_PASSWORD || "s3cret-admin";

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const text = async (p) => (await fetch(base + p)).text();

let fails = 0;
const check = (name, cond, d = "") => {
  console.log(`  ${cond ? "ok  " : "FAIL"}  ${name}${d ? "  " + d : ""}`);
  if (!cond) fails++;
};

console.log(`== pgforge admin UI (jsdom) -> ${base} ==`);

// Pull the actual served assets and run them in a jsdom window.
const html = (await text("/")).replace(/<script[^>]*><\/script>/g, "");
const petite = await text("/vendor/petite-vue.iife.js");
const appjs = await text("/app.js");

const dom = new JSDOM(html, { runScripts: "outside-only", url: base + "/", pretendToBeVisual: true });
const { window } = dom;
window.fetch = (u, o) => fetch(u.startsWith("http") ? u : base + u, o);
window.confirm = () => true;
window.eval(petite);
window.eval(appjs);
await sleep(200); // mount + @vue:mounted init()

const doc = window.document;
const Event = window.Event;
check("login screen rendered", !!doc.querySelector(".login"));

// fill credentials (v-model listens to 'input') and click sign-in
const inputs = doc.querySelectorAll(".login input");
inputs[0].value = EMAIL; inputs[0].dispatchEvent(new Event("input"));
inputs[1].value = PW;    inputs[1].dispatchEvent(new Event("input"));
doc.querySelector(".login button").dispatchEvent(new Event("click"));
await sleep(500); // login -> /schema -> /api/<first table>

const sidebar = doc.querySelector(".sidebar");
const grid = doc.querySelector(".main table");
check("logged in (topbar shown)", !!doc.querySelector(".topbar"));
check("sidebar lists products", !!sidebar && /products/.test(sidebar.textContent));
check("sidebar lists notes", !!sidebar && /notes/.test(sidebar.textContent));
check("sidebar hides internal pgf_ tables", !!sidebar && !/pgf_users/.test(sidebar.textContent));
check("data grid rendered", !!grid);

// switch to products and verify columns + rows show
const tableDivs = [...doc.querySelectorAll(".sidebar .tbl")];
const prod = tableDivs.find((d) => d.textContent.trim() === "products");
if (prod) { prod.dispatchEvent(new Event("click")); await sleep(400); }
const grid2 = doc.querySelector(".main table");
check("products grid has columns", !!grid2 && /price/.test(grid2.textContent) && /sku/.test(grid2.textContent));
check("products grid has rows", !!grid2 && /Cola 330ml/.test(grid2.textContent));

console.log(`\n== ui summary: ${fails ? fails + " FAILED" : "ALL PASS"} ==`);
process.exit(fails ? 1 : 0);
