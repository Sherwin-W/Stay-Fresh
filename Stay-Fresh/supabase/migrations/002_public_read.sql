-- Stay-Fresh migration 002: let the dashboard read WITHOUT signing in.
--
-- The dashboard now uses only the publishable (anon) key. Apply this in the
-- Supabase SQL editor, otherwise the dashboard shows "Couldn't load from the
-- database" because RLS only allowed the signed-in device account to read.
--
-- READ-ONLY and limited to one device. Anyone who has the publishable key
-- (it is shipped in the dashboard's JavaScript) can read this device's
-- readings. They still cannot insert, update or delete: the firmware keeps
-- signing in as the device account for writes.
-- Change 'fridge-05' if your DEVICE_ID differs.

grant select on public.fridge_readings to anon;

drop policy if exists "public read fridge-05" on public.fridge_readings;
create policy "public read fridge-05" on public.fridge_readings
  for select to anon
  using (device_id = 'fridge-05');

-- To undo:
--   drop policy "public read fridge-05" on public.fridge_readings;
--   revoke select on public.fridge_readings from anon;
