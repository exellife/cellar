// components/calendar.js — a reusable, domain-agnostic month calendar.
//
// Props:
//   events      [{ id, start:'YYYY-MM-DD', end?:'YYYY-MM-DD', title, color?, data? }]
//               (color is a free string; styled via .cal-ev.st-<color> if present)
//   modelValue  'YYYY-MM' focused month (optional v-model); defaults to today's month
//   weekStart   0=Sun .. 1=Mon (default 1)
//   maxPerDay   chips shown per day before "+N more" (default 3)
// Emits:
//   day-click(dateStr)      — empty-cell / day click (e.g. create on that day)
//   event-click(event)      — a chip click (the original event object, incl. .data)
//   update:modelValue(str)  — month navigation
import { ymd, todayStr, pad2 } from "/util.js";

// Build a 6×7 month grid (Date math only — local, never via date strings → TZ-safe).
function buildGrid(y, m, weekStart) {
  const first = new Date(y, m, 1);
  const lead = (first.getDay() - weekStart + 7) % 7;        // blanks before the 1st
  const start = new Date(y, m, 1 - lead);
  const weeks = [];
  for (let w = 0; w < 6; w++) {
    const row = [];
    for (let i = 0; i < 7; i++) {
      const d = new Date(start.getFullYear(), start.getMonth(), start.getDate() + w * 7 + i);
      row.push({ d: d.getDate(), date: ymd(d.getFullYear(), d.getMonth(), d.getDate()), inMonth: d.getMonth() === m });
    }
    weeks.push(row);
  }
  return weeks;
}

export const Calendar = {
  props: {
    events: { type: Array, default: () => [] },
    modelValue: { type: String, default: "" },
    weekStart: { type: Number, default: 1 },
    maxPerDay: { type: Number, default: 3 },
  },
  emits: ["day-click", "event-click", "update:modelValue"],
  data() {
    const valid = /^\d{4}-\d{2}$/.test(this.modelValue);
    const t = new Date();
    return { cur: valid
      ? { y: +this.modelValue.slice(0, 4), m: +this.modelValue.slice(5, 7) - 1 }
      : { y: t.getFullYear(), m: t.getMonth() } };
  },
  watch: {
    modelValue(v) { if (/^\d{4}-\d{2}$/.test(v)) this.cur = { y: +v.slice(0, 4), m: +v.slice(5, 7) - 1 }; },
  },
  computed: {
    today() { return todayStr(); },
    title() { return new Date(this.cur.y, this.cur.m, 1).toLocaleDateString(undefined, { month: "long", year: "numeric" }); },
    dows() { const b = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"]; return Array.from({ length: 7 }, (_, i) => b[(i + this.weekStart) % 7]); },
    weeks() {
      return buildGrid(this.cur.y, this.cur.m, this.weekStart).map(week => week.map(cell => {
        const all = this.events.filter(e => e.start <= cell.date && cell.date <= (e.end || e.start));
        return { ...cell, today: cell.date === this.today, events: all.slice(0, this.maxPerDay), more: Math.max(0, all.length - this.maxPerDay) };
      }));
    },
  },
  methods: {
    shift(delta) {
      let y = this.cur.y, m = this.cur.m + delta;
      while (m < 0) { m += 12; y--; }
      while (m > 11) { m -= 12; y++; }
      this.cur = { y, m };
      this.$emit("update:modelValue", `${y}-${pad2(m + 1)}`);
    },
    goToday() { const t = new Date(); this.cur = { y: t.getFullYear(), m: t.getMonth() }; this.$emit("update:modelValue", `${this.cur.y}-${pad2(this.cur.m + 1)}`); },
  },
  template: `
    <div class="cal">
      <div class="cal-head">
        <h2 class="cal-title">{{ title }}</h2>
        <div class="cal-nav">
          <button class="mini" @click="shift(-1)" aria-label="Previous month">‹</button>
          <button class="ghost small" @click="goToday">Today</button>
          <button class="mini" @click="shift(1)" aria-label="Next month">›</button>
        </div>
      </div>
      <div class="cal-dow">
        <div class="cal-dow-cell" v-for="d in dows" :key="d">{{ d }}</div>
      </div>
      <div class="cal-week" v-for="(week, wi) in weeks" :key="wi">
        <div class="cal-cell" v-for="cell in week" :key="cell.date"
             :class="{ out: !cell.inMonth, today: cell.today }" @click="$emit('day-click', cell.date)">
          <div class="cal-daynum">{{ cell.d }}</div>
          <div class="cal-ev" v-for="ev in cell.events" :key="ev.id"
               :class="ev.color ? 'st-'+ev.color : ''" :title="ev.title"
               @click.stop="$emit('event-click', ev)">{{ ev.title }}</div>
          <div class="cal-more" v-if="cell.more">+{{ cell.more }} more</div>
        </div>
      </div>
    </div>
  `,
};
