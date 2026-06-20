// Offline Notes — UI. All the sync logic lives in sync.js; this just renders and
// wires buttons to local writes (which optimistically update + queue for push).
import { Sync } from "/sync.js";

const sync = new Sync("notes");
const $ = (s) => document.querySelector(s);
const uuid = () => crypto.randomUUID();

// ---- sync log ----
sync.onlog = (m) => {
  const el = $("#log"); if (!el) return;
  const line = document.createElement("div");
  line.textContent = new Date().toLocaleTimeString() + "  " + m;
  el.prepend(line);
  while (el.childElementCount > 40) el.lastChild.remove();
};

// ---- render ----
sync.onchange = render;
function render() {
  $("#pending").textContent = `${sync.pending} pending`;
  $("#pending").classList.toggle("hot", sync.pending > 0);
  const wrap = $("#notes");
  wrap.replaceChildren();
  const rows = sync.liveRows().sort((a, b) => (b.updated_at || 0) - (a.updated_at || 0));
  if (!rows.length) wrap.innerHTML = '<p class="empty">No notes yet — hit “New note”.</p>';
  for (const r of rows) wrap.append(card(r));
}

function card(r) {
  const c = document.createElement("div");
  c.className = "note" + (r.rev === 0 ? " unsynced" : "");
  const t = document.createElement("input");
  t.className = "n-title"; t.value = r.title || ""; t.placeholder = "title";
  t.onchange = () => sync.put(r.id, { title: t.value, body: r.body });
  const b = document.createElement("textarea");
  b.className = "n-body"; b.value = r.body || ""; b.placeholder = "…";
  b.onchange = () => sync.put(r.id, { title: r.title, body: b.value });
  const meta = document.createElement("div");
  meta.className = "n-meta";
  meta.innerHTML = `<span>rev ${r.rev}${r.rev === 0 ? " · not yet synced" : ""}</span>`;
  const del = document.createElement("button");
  del.className = "n-del"; del.textContent = "✕"; del.title = "delete";
  del.onclick = () => sync.del(r.id);
  meta.append(del);
  c.append(t, b, meta);
  return c;
}

// ---- toolbar ----
$("#new").onclick = () => sync.put(uuid(), { title: "", body: "" });
$("#online").onchange = (e) => sync.setOnline(e.target.checked);
$("#sync-now").onclick = () => sync.sync();

// ---- auth ----
$("#auth-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  try { await sync.login($("#email").value.trim(), $("#password").value); await start(); }
  catch (err) {
    $("#auth-msg").textContent = err.status === 401 ? "Wrong email or password."
      : err.message || "Sign-in failed.";
  }
});
$("#register").onclick = async () => {
  const email = $("#email").value.trim(), password = $("#password").value;
  try {
    await sync._post("/auth/register", { email, password, role: "member" });
    await sync.login(email, password); await start();
  } catch (err) {
    $("#auth-msg").textContent = err.status === 403 ? "That role can't self-register."
      : (err.message || "Could not register.");
  }
};

async function start() {
  $("#auth").hidden = true; $("#app").hidden = false;
  $("#device").textContent = "device " + sync.device.slice(0, 6);
  $("#online").checked = sync.online;
  render();
  sync.startRealtime();
  await sync.sync();           // initial catch-up pull
}

if (sync.authed) start().catch(() => {});
