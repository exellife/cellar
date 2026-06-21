// app.js — ClerkHalls root: the shell (nav + sync bar + router-view), the router,
// and the auth gate. Built on Vue 3 + vue-router globals (vendored, no build step);
// data comes from the sync engine (db.state), shell/auth/modal state from the store.
import { db } from "/db.js";
import { store, startSync } from "/store.js";
import { SyncBar } from "/components/syncbar.js";
import { AppModal } from "/components/modal.js";
import { AuthView } from "/views/auth.js";
import { VenuesView } from "/views/venues.js";
import { BookingsView } from "/views/bookings.js";
import { BookingDetailView } from "/views/booking-detail.js";
import { MenuView } from "/views/menu.js";
import { SettingsView } from "/views/stubs.js";

const { createApp } = window.Vue;
const { createRouter, createWebHashHistory } = window.VueRouter;

const router = createRouter({
  history: createWebHashHistory(),
  linkActiveClass: "active",                 // so .navlink.active styling applies
  routes: [
    { path: "/", redirect: "/venues" },
    { path: "/venues", component: VenuesView },
    { path: "/bookings", component: BookingsView },
    { path: "/bookings/:id", component: BookingDetailView, props: true },
    { path: "/menu", component: MenuView },
    { path: "/settings", component: SettingsView },
  ],
});

const App = {
  components: { AuthView, SyncBar, AppModal },
  template: `
    <auth-view v-if="!authed"></auth-view>
    <div class="app" v-else>
      <aside class="nav">
        <div class="brand">ClerkHalls</div>
        <router-link class="navlink" to="/venues">Venues &amp; Halls</router-link>
        <router-link class="navlink" to="/bookings">Bookings</router-link>
        <router-link class="navlink" to="/menu">Menu</router-link>
        <router-link class="navlink" to="/settings">Settings</router-link>
        <div class="nav-foot">
          <sync-bar></sync-bar>
          <button class="ghost small" @click="signout">sign out</button>
        </div>
      </aside>
      <main class="content"><router-view></router-view></main>
    </div>
    <app-modal></app-modal>
  `,
  computed: { authed() { return store.authed; } },
  async mounted() { if (store.authed) await startSync(); },
  methods: { signout() { db.logout(); location.reload(); } },
};

createApp(App).use(router).mount("#app");
