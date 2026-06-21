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

// ---- dates (local-safe: never round-trip through UTC, so no off-by-one-day) ----
export const pad2 = (n) => String(n).padStart(2, "0");
export const ymd = (y, m, d) => `${y}-${pad2(m + 1)}-${pad2(d)}`;   // m is 0-11
export const todayStr = () => { const t = new Date(); return ymd(t.getFullYear(), t.getMonth(), t.getDate()); };
