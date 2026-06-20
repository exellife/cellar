// Venues & Rooms — the org → event_hall → hall hierarchy (full CRUD via sync).
import { db } from "/db.js";
import { el, clear, sortBy } from "/ui.js";

export const title = "Venues & Rooms";

export function render(root) {
  clear(root);
  const venues = db.all("event_halls").sort(sortBy("sort_order"));

  root.append(
    el("div", { class: "page-head" },
      el("h1", {}, "Venues & Rooms"),
      el("button", { class: "primary", onclick: () => editVenue() }, "+ Venue")),
    el("p", { class: "muted" },
      "A venue is a location; each venue holds one or more bookable rooms. Deleting a venue removes its rooms (cascades)."),
  );

  if (!venues.length) {
    root.append(el("p", { class: "empty" }, "No venues yet — add one to start."));
    return;
  }

  for (const v of venues) {
    const rooms = db.where("halls", r => r.event_hall_id === v.id).sort(sortBy("sort_order"));
    root.append(
      el("section", { class: "card venue" },
        el("div", { class: "venue-head" },
          el("div", {},
            el("h2", {}, v.name),
            el("div", { class: "muted small" }, [v.address, v.phone].filter(Boolean).join(" · ") || "—")),
          el("div", { class: "row-actions" },
            el("button", { class: "ghost", onclick: () => editVenue(v) }, "edit"),
            el("button", { class: "ghost danger", onclick: () => del("event_halls", v, `Delete venue "${v.name}" and its ${rooms.length} room(s)?`) }, "delete"))),
        el("div", { class: "rooms" },
          ...rooms.map(r => el("div", { class: "room" },
            el("span", { class: "room-name" }, r.name),
            el("span", { class: "muted small" }, r.capacity ? `cap ${r.capacity}` : ""),
            el("span", { class: "room-actions" },
              el("button", { class: "mini", onclick: () => editRoom(v.id, r) }, "✎"),
              el("button", { class: "mini danger", onclick: () => del("halls", r, `Delete room "${r.name}"?`) }, "✕")))),
          el("button", { class: "add-room", onclick: () => editRoom(v.id) }, "+ Room"))),
    );
  }
}

function del(table, row, confirmMsg) {
  if (confirm(confirmMsg)) db.remove(table, row.id);
}

// ---- modal editors ----
function editVenue(v) {
  modal(v ? "Edit venue" : "New venue", [
    ["name", "Name", v?.name || "", true],
    ["address", "Address", v?.address || ""],
    ["phone", "Phone", v?.phone || ""],
  ], (vals) => db.save("event_halls", { id: v?.id, ...vals, sort_order: v?.sort_order ?? db.count("event_halls") }));
}
function editRoom(venueId, r) {
  modal(r ? "Edit room" : "New room", [
    ["name", "Name", r?.name || "", true],
    ["capacity", "Capacity", r?.capacity || "", false, "number"],
  ], (vals) => db.save("halls", {
    id: r?.id, event_hall_id: venueId, name: vals.name,
    capacity: vals.capacity ? +vals.capacity : null,
    sort_order: r?.sort_order ?? db.count("halls", x => x.event_hall_id === venueId),
  }));
}

function modal(heading, fields, onsave) {
  const inputs = {};
  const body = el("div", { class: "modal-body" },
    el("h3", {}, heading),
    ...fields.map(([key, label, val, required, type]) => {
      const inp = el("input", { type: type || "text", value: val, placeholder: label });
      inputs[key] = () => inp.value.trim();
      return el("label", { class: "field" }, el("span", {}, label + (required ? " *" : "")), inp);
    }));
  const required = fields.filter(f => f[3]).map(f => f[0]);
  const back = el("div", { class: "modal-backdrop" },
    el("div", { class: "modal" }, body,
      el("div", { class: "modal-actions" },
        el("button", { class: "ghost", onclick: close }, "Cancel"),
        el("button", { class: "primary", onclick: save }, "Save"))));
  function close() { back.remove(); }
  function save() {
    const vals = {}; for (const k in inputs) vals[k] = inputs[k]();
    if (required.some(k => !vals[k])) return;
    onsave(vals); close();
  }
  document.body.append(back);
  back.querySelector("input")?.focus();
}
