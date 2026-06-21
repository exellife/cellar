// components/syncbar.js — the sidebar sync status: online dot, pending badge,
// online/offline toggle, and this browser's device id.
import { db } from "/db.js";

export const SyncBar = {
  template: `
    <div class="syncbar">
      <div class="syncrow"><span class="dot" :class="{ on: s.online }"></span><span class="small muted">{{ s.log }}</span></div>
      <span class="badge" :class="{ hot: s.pending }">{{ s.pending }} pending</span>
      <label class="switch small"><input type="checkbox" :checked="s.online" @change="toggle($event)"><span>online</span></label>
      <span class="small muted device">dev {{ dev }}</span>
    </div>
  `,
  computed: {
    s() { return db.state; },
    dev() { return db.device.slice(0, 6); },
  },
  methods: {
    toggle(e) { db.setOnline(e.target.checked); },
  },
};
