-- Stay-Fresh migration 001: additive telemetry columns.
--
-- * Purely additive: every new column is nullable, so existing rows and the
--   current firmware/dashboard keep working.
-- * Does NOT touch RLS, policies or grants. Table-level GRANTs already cover
--   new columns. If you ever switched to column-level grants, re-grant these.
-- * Apply in the Supabase SQL editor, THEN set CLOUD_SEND_EXTENDED=1 in
--   include/config.h and reflash. Doing it in the other order makes every
--   insert fail (unknown column).

alter table public.fridge_readings
  add column if not exists boot_id            text,
  add column if not exists seq                bigint,
  add column if not exists firmware_version   text,
  -- Device-side wall-clock time of the measurement (NULL if NTP was not synced).
  -- received_at remains the SERVER arrival time.
  add column if not exists measured_at        timestamptz,
  add column if not exists door_state         text
    check (door_state in ('closed','open','unknown')),
  add column if not exists closure_source     text
    check (closure_source in ('none','estimated','manual','reed')),
  add column if not exists calibration_state  text
    check (calibration_state in ('warmup','bias','awaiting_moves','calibrated','fault')),
  add column if not exists angle_quality      text
    check (angle_quality in ('none','estimated','degraded')),
  add column if not exists sensor_valid       boolean,
  add column if not exists since_reference_ms bigint check (since_reference_ms >= 0),
  add column if not exists missed_samples     bigint check (missed_samples >= 0),
  add column if not exists bias_adjustments   bigint check (bias_adjustments >= 0),
  add column if not exists reed_enabled       boolean,
  add column if not exists reed_closed        boolean;

comment on column public.fridge_readings.received_at is 'Server arrival time, not measurement time.';
comment on column public.fridge_readings.measured_at is 'Device clock (NTP) at measurement; NULL if unsynchronized.';
comment on column public.fridge_readings.door_state is 'closed | open | unknown. door_open=false does NOT imply closed when this is ''unknown''.';
comment on column public.fridge_readings.closure_source is 'How "closed" was established: estimated (gyro), manual (button), reed (contact).';

-- Dashboard queries filter by device and sort by time.
create index if not exists fridge_readings_device_received_idx
  on public.fridge_readings (device_id, received_at desc);

-- Verification (run manually; each should return the expected result):
--   select relrowsecurity from pg_class where oid = 'public.fridge_readings'::regclass;   -- true
--   select policyname, cmd, qual, with_check from pg_policies where tablename = 'fridge_readings';
--   select grantee, privilege_type from information_schema.role_table_grants
--     where table_name = 'fridge_readings' and grantee in ('anon','authenticated');
