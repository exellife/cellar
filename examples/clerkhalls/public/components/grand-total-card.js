// components/grand-total-card.js — the bill summary: section subtotals → grand
// total → discount (editable) → payments → balance due. Mirrors the Flutter
// GrandTotalCard. Discount is v-model:discount (emits update:discount).
import { som, prettyDate } from "/util.js";

export const GrandTotalCard = {
  props: {
    menu: { type: Number, default: 0 },
    wholesale: { type: Number, default: 0 },
    extras: { type: Number, default: 0 },
    discount: { type: Number, default: 0 },
    totalPaid: { type: Number, default: 0 },
    payments: { type: Array, default: () => [] },
  },
  emits: ["update:discount"],
  computed: {
    grandTotal() { return this.menu + this.wholesale + this.extras; },
    balance() { return this.grandTotal - this.discount - this.totalPaid; },
  },
  methods: {
    som, prettyDate,
    onDiscount(e) { this.$emit("update:discount", +e.target.value || 0); },
    payLabel(p) {
      const who = (p.source || "").trim() || p.method;
      return `${p.stage} · ${who} · ${prettyDate(p.paid_at)}`;
    },
  },
  template: `
    <div class="card bill">
      <div class="bline"><span>Menu</span><span>{{ som(menu) }}</span></div>
      <div class="bline"><span>Wholesale</span><span>{{ som(wholesale) }}</span></div>
      <div class="bline"><span>Extras</span><span>{{ som(extras) }}</span></div>
      <div class="bdiv"></div>
      <div class="bline grand"><span>Grand total</span><span>{{ som(grandTotal) }}</span></div>
      <div class="bline"><span>Discount</span>
        <input class="disc" type="number" :value="discount" @change="onDiscount" placeholder="0">
      </div>
      <div class="bline"><span>Payments</span>
        <span v-if="totalPaid > 0">− {{ som(totalPaid) }}</span>
        <span v-else class="muted small">no payments yet</span>
      </div>
      <div class="bpay" v-for="p in payments" :key="p.id">
        <span class="muted small">{{ payLabel(p) }}</span><span class="muted small">− {{ som(p.amount_kgs) }}</span>
      </div>
      <div class="bdiv"></div>
      <div class="bline balance"><span>Balance due</span><span>{{ som(balance) }}</span></div>
    </div>
  `,
};
