-- 0004_attr_labels.sql — attribute display labels belong in `labels`, not `label`.
--
-- The de-Russian pass (0001 baseline) stored each attribute's {en,ky} display map
-- as a JSON string INSIDE `label` (the plain-text fallback column), leaving the
-- dedicated `labels` column NULL. That's inconsistent with categories (name+labels)
-- and enum options (code+labels), and a KY-first client rendering `labels.ky ?? label`
-- falls back to `label` — showing the raw JSON string as the attribute name.
--
-- Fix: move the JSON into `labels`; reduce `label` to the plain English fallback.
-- Idempotent — once `label` is a plain string, json_type(label) <> 'object', so a
-- re-run matches no rows. Touches display columns only (no listing data).
UPDATE category_attribute
   SET labels = label,
       label  = json_extract(label, '$.en')
 WHERE json_valid(label) = 1
   AND json_type(label) = 'object'
   AND json_extract(label, '$.en') IS NOT NULL;
