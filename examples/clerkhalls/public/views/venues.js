// views/venues.js — Venues & Halls (org → venue → hall) full CRUD.
import { db } from "/db.js";
import { openModal } from "/store.js";
import { field, byOrder } from "/util.js";

export const VenuesView = {
  template: `
    <div>
      <div class="page-head"><h1>Venues &amp; Halls</h1><button class="primary" @click="newVenue">+ Venue</button></div>
      <p class="muted">A venue is a location; each holds one or more bookable halls. Deleting a venue removes its halls.</p>
      <p class="empty" v-if="!venues.length">No venues yet — add one to start.</p>
      <section class="card venue" v-for="v in venues" :key="v.id">
        <div class="venue-head">
          <div>
            <h2>{{ v.name }}</h2>
            <div class="muted small">{{ [v.address, v.phone].filter(Boolean).join(' · ') || '—' }}</div>
          </div>
          <div class="row-actions">
            <button class="ghost" @click="editVenue(v)">edit</button>
            <button class="ghost danger" @click="delVenue(v)">delete</button>
          </div>
        </div>
        <div class="rooms">
          <div class="room" v-for="h in halls(v.id)" :key="h.id">
            <span class="room-name">{{ h.name }}</span>
            <span class="muted small" v-if="h.capacity">cap {{ h.capacity }}</span>
            <span class="room-actions">
              <button class="mini" @click="editHall(v.id, h)">✎</button>
              <button class="mini danger" @click="delHall(h)">✕</button>
            </span>
          </div>
          <button class="add-room" @click="newHall(v.id)">+ Hall</button>
        </div>
      </section>
    </div>
  `,
  computed: {
    venues() { return [...db.state.venues].sort(byOrder); },
  },
  methods: {
    halls(vid) { return db.state.halls.filter(h => h.venue_id === vid).sort(byOrder); },
    newVenue() {
      openModal("New venue", [field("name", "Name", true), field("address", "Address"), field("phone", "Phone")],
        v => db.save("venues", { ...v, sort_order: db.state.venues.length }));
    },
    editVenue(x) {
      openModal("Edit venue",
        [field("name", "Name", true, x.name), field("address", "Address", false, x.address), field("phone", "Phone", false, x.phone)],
        v => db.save("venues", { id: x.id, ...v }));
    },
    delVenue(x) {
      const n = this.halls(x.id).length;
      if (confirm(`Delete venue "${x.name}"${n ? ` and its ${n} hall(s)` : ""}?`)) db.remove("venues", x.id);
    },
    newHall(vid) {
      openModal("New hall", [field("name", "Name", true), field("capacity", "Capacity", false, "", "number")],
        v => db.save("halls", { venue_id: vid, name: v.name, capacity: v.capacity ? +v.capacity : null, sort_order: this.halls(vid).length }));
    },
    editHall(vid, x) {
      openModal("Edit hall",
        [field("name", "Name", true, x.name), field("capacity", "Capacity", false, x.capacity, "number")],
        v => db.save("halls", { id: x.id, venue_id: vid, name: v.name, capacity: v.capacity ? +v.capacity : null }));
    },
    delHall(x) { if (confirm(`Delete hall "${x.name}"?`)) db.remove("halls", x.id); },
  },
};
