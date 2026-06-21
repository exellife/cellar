// util.js — shared helpers + domain enums used across views. No framework deps.

export const byOrder = (a, b) =>
  (a.sort_order ?? 0) - (b.sort_order ?? 0) || String(a.name || "").localeCompare(b.name || "");

// a modal field descriptor (see components/modal.js): text | number | date | select | textarea
export const field = (key, label, required, val, type, options) =>
  ({ key, label, required: !!required, val: val ?? "", type: type || "text", options });

export const opt = (value, label) => ({ value, label });

export const SESSIONS = [opt("morning", "Morning"), opt("afternoon", "Afternoon"), opt("evening", "Evening")];
export const STATUSES = [opt("tentative", "Tentative"), opt("confirmed", "Confirmed"), opt("completed", "Completed"), opt("cancelled", "Cancelled")];
export const EVENT_TYPES = [
  "wedding:Wedding", "bride_farewell:Bride farewell", "anniversary:Anniversary", "birthday:Birthday",
  "beshik_toi:Beshik toi", "zhentek_toi:Zhentek toi", "tushoo_toi:Tushoo toi", "quran_reading:Quran reading",
  "memorial:Memorial", "graduation:Graduation", "corporate:Corporate", "conference:Conference", "other:Other",
].map(s => { const [v, l] = s.split(":"); return opt(v, l); });

export const labelOf = (list, v) => (list.find(o => o.value === v) || {}).label || v || "";

// ---- booking status lifecycle ----
// Guided transitions: the buttons offered from each status (kind = button style).
export const STATUS_ACTIONS = {
  tentative: [{ to: "confirmed", label: "Confirm", kind: "primary" }, { to: "cancelled", label: "Cancel", kind: "danger" }],
  confirmed: [{ to: "completed", label: "Mark completed", kind: "primary" }, { to: "tentative", label: "Revert", kind: "ghost" }, { to: "cancelled", label: "Cancel", kind: "danger" }],
  completed: [{ to: "confirmed", label: "Reopen", kind: "ghost" }],
  cancelled: [{ to: "tentative", label: "Reopen", kind: "ghost" }],
};

// Derived calendar color (separate from raw status), per the Flutter app's
// booking_workflow_state: amber = unpriced, green = priced/ready, gray = done+paid,
// red = done but still owing. (Cancelled bookings are filtered out before this.)
export const workflowState = (booking, itemsSubtotal, totalPaid) => {
  if (booking.status === "completed") {
    return itemsSubtotal - (+booking.discount || 0) - totalPaid > 0 ? "red" : "gray";
  }
  return itemsSubtotal > 0 ? "green" : "amber";
};

// ---- dates (local-safe: never round-trip through UTC, so no off-by-one-day) ----
export const pad2 = (n) => String(n).padStart(2, "0");
export const ymd = (y, m, d) => `${y}-${pad2(m + 1)}-${pad2(d)}`;   // m is 0-11
export const todayStr = () => { const t = new Date(); return ymd(t.getFullYear(), t.getMonth(), t.getDate()); };

export const som = (n) => Math.round(+n || 0).toLocaleString() + " som";
export const prettyDate = (s) => {
  if (!s) return "";
  const [y, m, d] = String(s).slice(0, 10).split("-").map(Number);
  return new Date(y, m - 1, d).toLocaleDateString(undefined, { weekday: "short", month: "short", day: "numeric", year: "numeric" });
};
