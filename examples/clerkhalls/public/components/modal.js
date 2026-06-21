// components/modal.js — the generic form modal, driven by store.modal. Renders one
// input per field descriptor (text | number | date | select | textarea). A view
// opens it via openModal(title, fields, save); save() may return false to abort
// (e.g. a declined booking conflict) and keep the modal open.
import { store, closeModal, saveModal } from "/store.js";

export const AppModal = {
  template: `
    <div class="modal-backdrop" v-if="modal" @click="close">
      <div class="modal" @click.stop>
        <h3>{{ modal.title }}</h3>
        <label class="field" v-for="f in modal.fields" :key="f.key">
          <span>{{ f.label }}<template v-if="f.required"> *</template></span>
          <select v-if="f.type==='select'" v-model="modal.model[f.key]">
            <option value="" disabled>Choose…</option>
            <option v-for="o in f.options" :value="o.value">{{ o.label }}</option>
          </select>
          <textarea v-else-if="f.type==='textarea'" v-model="modal.model[f.key]" rows="3" :placeholder="f.label"></textarea>
          <input v-else :type="f.type" v-model="modal.model[f.key]" :placeholder="f.label" @keyup.enter="save">
        </label>
        <div class="modal-actions">
          <button class="ghost" @click="close">Cancel</button>
          <button class="primary" @click="save">Save</button>
        </div>
      </div>
    </div>
  `,
  computed: { modal() { return store.modal; } },
  methods: { close: closeModal, save: saveModal },
};
