// store.js — app-level reactive UI state + actions shared across components.
// (db.state holds the synced data; this holds the shell/auth/modal UI state.)
import { db } from "/db.js";

const { reactive } = window.Vue;

export const store = reactive({
  authed: db.authed,     // toggled by sign-in / dropped on a 401 (session expired)
  authMsg: "",
  modal: null,           // { title, fields, save, model } — see components/modal.js
});

// ---- generic modal editor ----
export function openModal(title, fields, save) {
  store.modal = { title, fields, save, model: Object.fromEntries(fields.map(f => [f.key, f.val])) };
}
export function closeModal() { store.modal = null; }
export function saveModal() {
  const m = store.modal;
  if (m.fields.some(f => f.required && !String(m.model[f.key] ?? "").trim())) return;
  if (m.save({ ...m.model }) === false) return;   // save aborted (e.g. conflict declined) — keep open
  store.modal = null;
}

// ---- sync lifecycle (idempotent: safe to call again after re-login) ----
let started = false;
export async function startSync() {
  if (!started) {
    started = true;
    // a stale/expired token (server restarted) -> drop to the login screen.
    db.onAuthLost = () => { store.authed = false; store.authMsg = "Session expired — please sign in again."; };
    db.startRealtime();                      // instant updates while the WS is connected
    setInterval(() => db.sync(), 12000);     // safety net: converge even if a realtime event is missed
    window.addEventListener("focus", () => db.sync());
    document.addEventListener("visibilitychange", () => { if (!document.hidden) db.sync(); });
  }
  await db.sync();                           // initial / post-login catch-up
}
