# Taxonomy — English-first working spec

One file per top category. **Polish the English labels here first** (no Russian), then a second
pass swaps in Kyrgyz where it reads better (right in these files). A loader reads these and PATCHes
the cellar (`labels.ky`) — so the workflow is **edit files → run loader**, no chat ping-pong.

## Format
```
Category: **English Label**  ·  `cat-id`

## Subcategory Label · `cat-id`
- **Label** `attr_key` (type[, unit]): opt · opt · opt
```
- **`backtick` tokens are cellar ids/keys** — the loader maps each line by these. **Don't edit them.**
- Options are listed **in cellar order** and `·`-separated (loader matches by position).
- The option *codes* in the DB stay as-is; we only set the **display label**. `(bool)` / `(text)` attrs have no options.

## Status
- `transport` · `real-estate` · `electronics` — **already loaded** (with a KY pass); files reflect the English source.
- `home-garden` · `personal-items` · `animals` — English drafts, not yet loaded.
- `jobs` · `services` — no subcategories yet (deferred taxonomy).
