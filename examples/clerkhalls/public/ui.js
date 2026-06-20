// ui.js — tiny DOM helpers (no framework).
export function el(tag, attrs = {}, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class") e.className = v;
    else if (k === "html") e.innerHTML = v;
    else if (k.startsWith("on") && typeof v === "function") e.addEventListener(k.slice(2), v);
    else if (v === true) e.setAttribute(k, "");
    else if (v != null && v !== false) e.setAttribute(k, v);
  }
  for (const kid of kids.flat()) if (kid != null && kid !== false)
    e.append(kid.nodeType ? kid : document.createTextNode(String(kid)));
  return e;
}
export const clear = (n) => { n.replaceChildren(); return n; };
export const fmtKgs = (n) => (Math.round((+n || 0))).toLocaleString("ru-RU") + " som";
export const sortBy = (k) => (a, b) => (a[k] ?? 0) - (b[k] ?? 0) || String(a.name||"").localeCompare(b.name||"");
