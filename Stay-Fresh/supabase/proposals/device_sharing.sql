-- PROPOSAL ONLY: NOT applied, NOT required for the current setup.
--
-- Today every row's owner_id is the DEVICE account, and the SELECT policy is
-- owner_id = auth.uid(). So a browser can read data only after signing in
-- AS THAT SAME ACCOUNT. That works for a class demo but means typing the
-- device's password into a browser. If humans need their own logins, grant
-- access explicitly per device instead of opening the table to all users:

create table if not exists public.device_viewers (
  device_id text not null,
  viewer_id uuid not null references auth.users(id) on delete cascade,
  primary key (device_id, viewer_id)
);
alter table public.device_viewers enable row level security;

-- Viewers may see only their own grants. No client INSERT/UPDATE/DELETE:
-- an admin adds rows from the SQL editor:
--   insert into public.device_viewers values ('fridge-05', '<viewer auth.users id>');
create policy "viewers read own grants" on public.device_viewers
  for select to authenticated using (viewer_id = auth.uid());
revoke all on public.device_viewers from anon;
grant select on public.device_viewers to authenticated;

-- Additional (OR-ed with the existing owner policy) read access:
create policy "viewers read granted device readings" on public.fridge_readings
  for select to authenticated
  using (exists (
    select 1 from public.device_viewers v
    where v.viewer_id = auth.uid() and v.device_id = fridge_readings.device_id
  ));
