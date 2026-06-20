-- on_realtime delivery filter for realtime_filter_test.py: suppress any change
-- whose note title starts with 'SILENT' (selected via CEL_HOOKS_FILE).
function on_realtime(change, subscriber)
  local row = change.row
  if row and row.title and tostring(row.title):match('^SILENT') then
    return false        -- do not deliver SILENT notes to anyone
  end
  return true
end
