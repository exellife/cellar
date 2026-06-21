// components/payment-section.js — list + add payments for a booking (deposit +
// installments). Its own component so it can be role-gated later. Controlled:
// emits add(payment) / remove(id); the parent persists to the payments table.
import { som, prettyDate, todayStr } from "/util.js";

const STAGES = ["deposit", "installment", "final"];
const METHODS = ["cash", "mobile", "card", "transfer"];

export const PaymentSection = {
  props: {
    payments: { type: Array, default: () => [] },
    newId: { type: Function, required: true },
  },
  emits: ["add", "remove"],
  data() {
    return { STAGES, METHODS, adding: false, d: this.blank() };
  },
  computed: {
    total() { return this.payments.reduce((s, p) => s + (+p.amount_kgs || 0), 0); },
    sorted() { return [...this.payments].sort((a, b) => String(a.paid_at).localeCompare(b.paid_at)); },
  },
  methods: {
    som, prettyDate,
    blank() { return { amount: "", method: "cash", stage: "deposit", paid_at: todayStr(), source: "" }; },
    open() { this.adding = true; this.d = this.blank(); },
    submit() {
      const amt = +this.d.amount || 0;
      if (amt <= 0) return;
      this.$emit("add", {
        id: this.newId(), amount: amt, amount_kgs: amt, currency: "KGS",
        method: this.d.method, stage: this.d.stage,
        paid_at: this.d.paid_at || todayStr(), source: this.d.source.trim() || null,
      });
      this.adding = false;
    },
    remove(p) { if (confirm(`Remove this ${som(p.amount_kgs)} payment?`)) this.$emit("remove", p.id); },
  },
  template: `
    <div class="card">
      <div class="sec-head"><h3>Payments</h3><span class="muted small">total {{ som(total) }}</span></div>
      <p class="empty small" v-if="!sorted.length && !adding">No payments recorded.</p>
      <div class="pay-row" v-for="p in sorted" :key="p.id">
        <div>
          <span class="pill st-confirmed">{{ p.stage }}</span>
          <span class="muted small">{{ (p.source||p.method) }} · {{ prettyDate(p.paid_at) }}</span>
        </div>
        <div class="pay-amt">{{ som(p.amount_kgs) }}<button class="mini danger" @click="remove(p)">✕</button></div>
      </div>

      <div class="pay-add" v-if="adding">
        <input type="number" v-model="d.amount" placeholder="Amount" @keyup.enter="submit">
        <select v-model="d.stage"><option v-for="s in STAGES" :value="s">{{ s }}</option></select>
        <select v-model="d.method"><option v-for="m in METHODS" :value="m">{{ m }}</option></select>
        <input type="date" v-model="d.paid_at">
        <input v-model="d.source" placeholder="Source (e.g. MBank)">
        <button class="primary small" @click="submit">Add</button>
        <button class="ghost small" @click="adding=false">Cancel</button>
      </div>
      <button v-else class="ghost small" @click="open">+ Payment</button>
    </div>
  `,
};
