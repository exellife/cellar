// views/auth.js — the sign-in / register screen shown when not authenticated.
import { db } from "/db.js";
import { store, startSync } from "/store.js";

export const AuthView = {
  template: `
    <div class="auth">
      <div class="brand big">ClerkHalls</div>
      <p class="muted">Sign in to your organization. Open a second browser as another device to see live sync.</p>
      <div class="card">
        <label class="field"><span>Email</span><input v-model="email" @keyup.enter="signin(false)"></label>
        <label class="field"><span>Password</span><input type="password" v-model="password" @keyup.enter="signin(false)"></label>
        <div class="modal-actions">
          <button class="ghost" @click="signin(true)">Create account</button>
          <button class="primary" @click="signin(false)">Sign in</button>
        </div>
        <p class="msg">{{ authMsg }}</p>
      </div>
    </div>
  `,
  data() {
    return {
      email: localStorage.getItem("ch_email") || "owner@clerkhalls.local",
      password: "clerkhalls",
    };
  },
  computed: { authMsg() { return store.authMsg; } },
  methods: {
    async signin(register) {
      try {
        if (register) await db.register(this.email.trim(), this.password);
        await db.login(this.email.trim(), this.password);
        store.authed = true;
        await startSync();
      } catch (e) {
        store.authMsg = e.status === 401 ? "Wrong email or password." : (e.message || "Sign-in failed.");
      }
    },
  },
};
