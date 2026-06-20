// Tasklets — the front-end. Pure ES modules, no build step, no dependencies.
// All it does is call the cellar REST API and react to realtime CHANGE events.
import { Cellar } from "/cellar.js";

const cel = new Cellar();
const STATUSES = ["todo", "doing", "done"];
const PRIO = { 1: "P1", 2: "P2", 3: "P3", 4: "P4", 5: "P5" };
let tasks = [];        // local mirror of my visible tasks
let rt = null;         // realtime subscription handle

const $ = (s) => document.querySelector(s);
const el = (tag, cls, txt) => { const e = document.createElement(tag); if (cls) e.className = cls; if (txt != null) e.textContent = txt; return e; };

// ---- auth UI ---------------------------------------------------------------
let mode = "login";
document.querySelectorAll(".tab").forEach((t) =>
  t.addEventListener("click", () => {
    mode = t.dataset.tab;
    document.querySelectorAll(".tab").forEach((x) => x.classList.toggle("active", x === t));
    $("#auth-submit").textContent = mode === "login" ? "Sign in" : "Create account";
    $("#password").autocomplete = mode === "login" ? "current-password" : "new-password";
    $("#auth-msg").textContent = "";
  }));

$("#auth-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const email = $("#email").value.trim(), password = $("#password").value;
  try {
    if (mode === "register") await cel.register(email, password, "member");
    await cel.login(email, password);     // register may not return a session; log in explicitly
    localStorage.setItem("cel_email", email);
    await start();
  } catch (err) {
    $("#auth-msg").textContent = err.status === 401 ? "Wrong email or password."
      : err.status === 403 ? "That role can't self-register here."
      : (err.message || "Something went wrong.");
  }
});

$("#logout").addEventListener("click", () => {
  cel.logout(); rt && rt.close(); rt = null;
  $("#app").hidden = true; $("#auth").hidden = false; $("#who").hidden = true;
  $("#password").value = "";
});

// ---- the board -------------------------------------------------------------
$("#new-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const title = $("#new-title").value.trim();
  if (!title) return;
  try {
    await cel.create("tasks", { title, priority: +$("#new-priority").value });
    $("#new-title").value = "";
    // No manual refresh needed — the realtime CHANGE will repaint. But also refetch
    // so it feels instant even if realtime is disabled.
    await reload();
  } catch (err) {
    // authorize() rejects P5 (urgent) from non-admins; before() rejects an empty title.
    flash(err.status === 403 ? "Urgent (P5) is reserved for admins." : (err.message || "Could not add task."));
  }
});

$("#clear-done").addEventListener("click", async () => {
  const { result } = await cel.rpc("clear_done");
  flash(`cleared ${result.cleared} done task(s)`);
  await reload();
});

async function move(task, dir) {
  const i = STATUSES.indexOf(task.status) + dir;
  if (i < 0 || i >= STATUSES.length) return;
  await cel.update("tasks", task.id, { status: STATUSES[i] });
  await reload();
}

async function del(task) {
  try {
    await cel.remove("tasks", task.id);   // policy scopes this to your own tasks
    await reload();
  } catch (err) {
    flash(err.message || "Could not delete.");
  }
}

function card(task) {
  const c = el("div", "taskcard prio-" + task.priority);
  c.append(el("span", "prio", PRIO[task.priority] || "P3"));
  c.append(el("span", "title", task.title));
  const actions = el("div", "actions");
  if (task.status !== "todo") { const b = el("button", "mv", "◀"); b.onclick = () => move(task, -1); actions.append(b); }
  if (task.status !== "done") { const b = el("button", "mv", "▶"); b.onclick = () => move(task, +1); actions.append(b); }
  const x = el("button", "rm", "✕"); x.title = "delete";
  x.onclick = () => del(task); actions.append(x);
  c.append(actions);
  return c;
}

function render() {
  for (const s of STATUSES) $("#col-" + s).replaceChildren();
  for (const t of [...tasks].sort((a, b) => b.priority - a.priority || a.created_at - b.created_at)) {
    const col = $("#col-" + t.status);
    if (col) col.append(card(t));
  }
}

async function reload() {
  const { rows } = await cel.list("tasks", { order: "-priority", limit: "200" });
  tasks = rows;
  render();
  refreshStats();
}

async function refreshStats() {
  try {
    const { result } = await cel.rpc("board_stats");      // hooks.lua rpc
    for (const s of STATUSES) {
      const span = document.querySelector(`[data-stat="${s}"]`);
      if (span) span.textContent = `${s} ${result[s] ?? 0}`;
    }
  } catch { /* stats are best-effort */ }
}

// ---- realtime --------------------------------------------------------------
function startRealtime() {
  const dot = $("#rtdot");
  rt = cel.subscribe("tasks", (change) => {
    dot.classList.add("on"); dot.title = "realtime: live";
    applyChange(change);
  });
  // optimistic: mark connecting; the first event flips it green
  dot.classList.remove("on"); dot.title = "realtime: connecting…";
}

function applyChange({ op, row }) {
  if (!row) return;
  const i = tasks.findIndex((t) => t.id === row.id);
  if (op === "DELETE") { if (i >= 0) tasks.splice(i, 1); }
  else if (i >= 0)     { tasks[i] = row; }
  else                 { tasks.push(row); }
  render();
  refreshStats();
}

function flash(msg) {
  const f = el("div", "flash", msg);
  document.body.append(f);
  setTimeout(() => f.remove(), 2600);
}

// ---- boot ------------------------------------------------------------------
async function start() {
  $("#auth").hidden = true; $("#app").hidden = false; $("#who").hidden = false;
  $("#whoami").textContent = localStorage.getItem("cel_email") || "signed in";
  await reload();
  startRealtime();
}

// resume a stored session on reload
if (cel.authed) start().catch(() => { cel.logout(); });
