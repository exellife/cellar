// app.js — ClerkHalls shell: auth, nav, hash router, sync status bar.
import { db } from "/db.js";
import { el, clear } from "/ui.js";
import * as venues from "/views/venues.js";

const stub = (name) => ({ title: name, render(root) {
  clear(root).append(el("div", { class: "page-head" }, el("h1", {}, name)),
    el("p", { class: "empty" }, "Coming in the next iteration.")); } });

const ROUTES = {
  venues,
  bookings: stub("Bookings"),
  menu: stub("Menu catalog"),
  settings: stub("Settings"),
};
const NAV = [["venues", "Venues & Rooms"], ["bookings", "Bookings"], ["menu", "Menu"], ["settings", "Settings"]];

const $ = (s) => document.querySelector(s);

function route() { return (location.hash.replace(/^#\/?/, "") || "venues").split("/")[0]; }

function renderShell() {
  const r = route(); const view = ROUTES[r] || venues;
  document.body.innerHTML = "";
  document.body.append(
    el("div", { class: "app" },
      el("aside", { class: "nav" },
        el("div", { class: "brand" }, "ClerkHalls"),
        ...NAV.map(([key, label]) =>
          el("a", { href: "#/" + key, class: "navlink" + (r === key ? " active" : "") }, label)),
        el("div", { class: "nav-foot" },
          syncBar(),
          el("button", { class: "ghost small", onclick: () => { db.logout(); location.reload(); } }, "sign out"))),
      el("main", { class: "content", id: "content" })));
  view.render($("#content"));
}

function syncBar() {
  const dot = el("span", { class: "dot" + (db.online ? " on" : "") });
  const status = el("span", { class: "small muted" }, db._log || (db.online ? "online" : "offline"));
  const pending = el("span", { class: "badge" + (db.pending ? " hot" : "") }, `${db.pending} pending`);
  const toggle = el("label", { class: "switch small" },
    el("input", { type: "checkbox", checked: db.online ? true : false,
                  onchange: (e) => db.setOnline(e.target.checked) }),
    el("span", {}, "online"));
  return el("div", { class: "syncbar" }, el("div", { class: "syncrow" }, dot, status), pending, toggle,
    el("span", { class: "small muted device" }, "dev " + db.device.slice(0, 6)));
}

// ---- auth gate ----
function renderAuth(msg) {
  document.body.innerHTML = "";
  const email = el("input", { type: "email", placeholder: "email", value: localStorage.getItem("ch_email") || "owner@clerkhalls.local", autocomplete: "username" });
  const pass = el("input", { type: "password", placeholder: "password (8+)", value: "clerkhalls", autocomplete: "current-password" });
  const note = el("p", { class: "msg" }, msg || "");
  const go = async (register) => {
    try { if (register) await db.register(email.value.trim(), pass.value); await db.login(email.value.trim(), pass.value); boot(); }
    catch (e) { note.textContent = e.status === 401 ? "Wrong email or password." : (e.message || "Failed."); }
  };
  document.body.append(el("div", { class: "auth" },
    el("div", { class: "brand big" }, "ClerkHalls"),
    el("p", { class: "muted" }, "Sign in to your organization. Open a second browser as another device to see live sync."),
    el("div", { class: "card" },
      el("label", { class: "field" }, el("span", {}, "Email"), email),
      el("label", { class: "field" }, el("span", {}, "Password"), pass),
      el("div", { class: "modal-actions" },
        el("button", { class: "ghost", onclick: () => go(true) }, "Create account"),
        el("button", { class: "primary", onclick: () => go(false) }, "Sign in")),
      note)));
}

// ---- boot ----
let booted = false;
async function boot() {
  if (booted) { renderShell(); return; }
  booted = true;
  db.on(() => { if (booted) updateSyncBar(); });   // re-render the sync bar on changes
  window.addEventListener("hashchange", renderShell);
  renderShell();
  db.startRealtime();
  await db.sync();
  renderShell();
}
function updateSyncBar() {
  const old = $(".nav-foot"); if (!old) return;
  // cheap: re-render the whole current view + bar (data changed)
  const view = ROUTES[route()] || venues;
  const c = $("#content"); if (c) view.render(c);
  const bar = $(".syncbar"); if (bar) bar.replaceWith(syncBar());
}

if (db.authed) boot(); else renderAuth();
