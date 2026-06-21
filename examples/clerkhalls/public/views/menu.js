// views/menu.js — the Menu catalog: menu_categories → menu_items (name + price),
// the org's reusable menu. Items here are pickable when building a booking's Menu.
// Uses the itemized table in catalog mode (no qty/total). Persists offline-first.
import { db } from "/db.js";
import { byOrder, opt, som } from "/util.js";
import { ItemizedTable } from "/components/itemized-table.js";

export const MenuView = {
  components: { ItemizedTable },
  computed: {
    cats() {
      return [...db.state.menu_categories].sort(byOrder).map((c) => ({ id: c.id, name: c.name, sort_order: c.sort_order }));
    },
    rows() {
      return db.state.menu_items.map((i) => ({ id: i.id, category_id: i.category_id, name: i.name, quantity: 1, unit_price: i.price_kgs }));
    },
  },
  methods: {
    som,
    uuid() { return crypto.randomUUID(); },
    countIn(catId) { return db.state.menu_items.filter((i) => i.category_id === catId).length; },
    addItem(r) { db.save("menu_items", { id: r.id, category_id: r.category_id, name: r.name, price_kgs: r.unit_price, sort_order: this.countIn(r.category_id) }); },
    editItem(r) { db.save("menu_items", { id: r.id, category_id: r.category_id, name: r.name, price_kgs: r.unit_price }); },
    removeItem(id) { db.remove("menu_items", id); },
    addCat(name) { db.save("menu_categories", { name, sort_order: db.state.menu_categories.length }); },
    renameCat({ id, name }) { db.save("menu_categories", { id, name }); },
    removeCat(id) { db.remove("menu_categories", id); },   // FK cascade removes its items
  },
  template: `
    <div>
      <div class="page-head"><h1>Menu catalog</h1></div>
      <p class="muted">Your reusable menu. Add categories and dishes with prices — they become pickable when building a booking's Menu.</p>
      <itemized-table :rows="rows" :categories="cats" :catalog="true" :money="som" :new-id="uuid"
        @row-add="addItem" @row-edit="editItem" @row-remove="removeItem"
        @category-add="addCat" @category-rename="renameCat" @category-remove="removeCat"></itemized-table>
    </div>
  `,
};
