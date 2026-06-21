// components/itemized-table.js — reusable (item · qty · per-unit · total) table.
// Categorized by default (rows grouped under category headers); set
// :categorized="false" for a flat list. Controlled: the parent owns the data
// and persists each change, so it works offline-first (one db.save per row).
//
// Props
//   rows        [{ id, category_id, name, quantity, unit_price }]
//   categories  [{ id, name, sort_order }]   (used when categorized)
//   categorized Boolean (default true)
//   money       (n) => string                (formatter; default rounded number)
//   newId       () => string                 (id allocator, e.g. crypto.randomUUID)
// Emits
//   row-add(row) · row-edit(row) · row-remove(id)
//   category-add(name) · category-rename({id,name}) · category-remove(id)
const num = (v) => +String(v).trim() || 0;
const valid = (d, catalog = false) => d.name.trim() && num(d.unit_price) >= 0 && (catalog || num(d.quantity) > 0);

export const ItemizedTable = {
  props: {
    rows: { type: Array, default: () => [] },
    categories: { type: Array, default: () => [] },
    categorized: { type: Boolean, default: true },
    catalog: { type: Boolean, default: false },   // template rows: name + price only (no qty/total/subtotal)
    suggestions: { type: Array, default: () => [] },   // [{name, price}] catalog autocomplete for the item name
    money: { type: Function, default: (n) => Math.round(n).toLocaleString() },
    newId: { type: Function, required: true },
  },
  emits: ["row-add", "row-edit", "row-remove", "category-add", "category-rename", "category-remove"],
  data() {
    return {
      editingId: null, editDraft: { name: "", quantity: 1, unit_price: "" },
      addingFor: null, addDraft: { name: "", quantity: 1, unit_price: "" },
      renamingId: null, catDraft: "",
      addingCat: false, newCatName: "",
      listId: "dl-" + Math.random().toString(36).slice(2, 9),
    };
  },
  computed: {
    cats() {
      return [...this.categories].sort((a, b) =>
        (a.sort_order ?? 0) - (b.sort_order ?? 0) || String(a.name).localeCompare(b.name));
    },
    knownCatIds() { return new Set(this.cats.map((c) => c.id)); },
    orphans() { return this.rows.filter((r) => !r.category_id || !this.knownCatIds.has(r.category_id)); },
    subtotal() { return this.rows.reduce((s, r) => s + this.lineTotal(r), 0); },
    editValid() { return valid(this.editDraft, this.catalog); },
    addValid() { return valid(this.addDraft, this.catalog); },
  },
  methods: {
    lineTotal(r) { return num(r.quantity) * num(r.unit_price); },
    live(d) { return num(d.quantity) * num(d.unit_price); },   // live total for a draft
    rowsOf(catId) { return this.rows.filter((r) => r.category_id === catId); },
    onName(draft) {   // catalog pick: prefill unit price when the name matches a suggestion
      const hit = this.suggestions.find((s) => s.name.toLowerCase() === draft.name.trim().toLowerCase());
      if (hit && !String(draft.unit_price).trim()) draft.unit_price = hit.price;
    },

    startEdit(r) { this.editingId = r.id; this.editDraft = { name: r.name, quantity: r.quantity, unit_price: r.unit_price }; this.addingFor = null; },
    saveEdit(r) {
      if (!this.editValid) return;
      this.$emit("row-edit", { ...r, name: this.editDraft.name.trim(), quantity: num(this.editDraft.quantity), unit_price: num(this.editDraft.unit_price) });
      this.editingId = null;
    },
    removeRow(r) { if (confirm(`Remove "${r.name}"?`)) this.$emit("row-remove", r.id); },

    startAdd(catId) { this.addingFor = catId; this.addDraft = { name: "", quantity: 1, unit_price: "" }; this.editingId = null; },
    submitAdd(catId) {
      if (!this.addValid) return;
      this.$emit("row-add", {
        id: this.newId(), category_id: catId === "_flat" ? null : catId,
        name: this.addDraft.name.trim(), quantity: num(this.addDraft.quantity), unit_price: num(this.addDraft.unit_price),
      });
      this.addDraft = { name: "", quantity: 1, unit_price: "" };   // keep open for rapid entry
    },

    startRename(c) { this.renamingId = c.id; this.catDraft = c.name; },
    saveRename(c) { const n = this.catDraft.trim(); if (n) this.$emit("category-rename", { id: c.id, name: n }); this.renamingId = null; },
    removeCategory(c) {
      const n = this.rowsOf(c.id).length;
      if (confirm(`Remove category "${c.name}"?${n ? ` Its ${n} item(s) move to Uncategorized.` : ""}`)) this.$emit("category-remove", c.id);
    },
    submitCat() { const n = this.newCatName.trim(); if (n) this.$emit("category-add", n); this.newCatName = ""; this.addingCat = false; },
  },
  template: `
    <div class="itable">
      <datalist :id="listId" v-if="suggestions.length"><option v-for="s in suggestions" :key="s.name" :value="s.name"></option></datalist>
      <div class="itable-head">
        <div class="ic-item">Item</div><div class="ic-qty" v-if="!catalog">Qty</div>
        <div class="ic-unit">{{ catalog ? 'Price' : 'Per unit' }}</div>
        <div class="ic-total" v-if="!catalog">Total</div><div class="ic-act"></div>
      </div>

      <template v-for="block in (categorized ? cats : [{ id: '_flat', name: null }])" :key="block.id">
        <div class="itable-cat" v-if="categorized">
          <input v-if="renamingId===block.id" v-model="catDraft" @keyup.enter="saveRename(block)" @keyup.esc="renamingId=null" @blur="saveRename(block)">
          <span v-else class="cat-name" @click="startRename(block)">{{ block.name }}</span>
          <button class="mini danger" @click="removeCategory(block)">✕</button>
        </div>

        <template v-for="r in (block.id==='_flat' ? rows : rowsOf(block.id))" :key="r.id">
          <div class="itable-row" v-if="editingId!==r.id">
            <div class="ic-item">{{ r.name }}</div>
            <div class="ic-qty" v-if="!catalog">{{ r.quantity }}</div>
            <div class="ic-unit">{{ money(+r.unit_price||0) }}</div>
            <div class="ic-total" v-if="!catalog">{{ money(lineTotal(r)) }}</div>
            <div class="ic-act"><button class="mini" @click="startEdit(r)">✎</button><button class="mini danger" @click="removeRow(r)">✕</button></div>
          </div>
          <div class="itable-row editing" v-else>
            <div class="ic-item"><input v-model="editDraft.name" :list="listId" @input="onName(editDraft)" placeholder="Item"></div>
            <div class="ic-qty" v-if="!catalog"><input type="number" v-model="editDraft.quantity"></div>
            <div class="ic-unit"><input type="number" v-model="editDraft.unit_price"></div>
            <div class="ic-total" v-if="!catalog">{{ money(live(editDraft)) }}</div>
            <div class="ic-act"><button class="mini" :disabled="!editValid" @click="saveEdit(r)">✓</button><button class="mini" @click="editingId=null">✕</button></div>
          </div>
        </template>

        <div class="itable-row add" v-if="addingFor===block.id">
          <div class="ic-item"><input v-model="addDraft.name" :list="listId" @input="onName(addDraft)" placeholder="Item" @keyup.enter="submitAdd(block.id)"></div>
          <div class="ic-qty" v-if="!catalog"><input type="number" v-model="addDraft.quantity"></div>
          <div class="ic-unit"><input type="number" v-model="addDraft.unit_price" @keyup.enter="submitAdd(block.id)"></div>
          <div class="ic-total" v-if="!catalog">{{ money(live(addDraft)) }}</div>
          <div class="ic-act"><button class="mini" :disabled="!addValid" @click="submitAdd(block.id)">＋</button><button class="mini" @click="addingFor=null">✕</button></div>
        </div>
        <button v-else class="itable-additem" @click="startAdd(block.id)">+ item</button>
      </template>

      <template v-if="categorized && orphans.length">
        <div class="itable-cat"><span class="cat-name muted">Uncategorized</span></div>
        <div class="itable-row" v-for="r in orphans" :key="r.id">
          <div class="ic-item">{{ r.name }}</div><div class="ic-qty" v-if="!catalog">{{ r.quantity }}</div>
          <div class="ic-unit">{{ money(+r.unit_price||0) }}</div><div class="ic-total" v-if="!catalog">{{ money(lineTotal(r)) }}</div>
          <div class="ic-act"><button class="mini" @click="startEdit(r)">✎</button><button class="mini danger" @click="removeRow(r)">✕</button></div>
        </div>
      </template>

      <div class="itable-addcat" v-if="categorized">
        <input v-if="addingCat" v-model="newCatName" placeholder="New category" @keyup.enter="submitCat" @keyup.esc="addingCat=false">
        <button v-if="addingCat" class="mini" @click="submitCat">✓</button>
        <button v-else class="link" @click="addingCat=true">+ Add category</button>
      </div>

      <div class="itable-foot" v-if="!catalog"><span>Subtotal</span><span>{{ money(subtotal) }}</span></div>
    </div>
  `,
};
