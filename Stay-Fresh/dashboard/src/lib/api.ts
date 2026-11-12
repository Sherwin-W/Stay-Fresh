import type { SupabaseClient } from "@supabase/supabase-js";
import { PAGE_SIZE, normalizeRow } from "./derive";
import type { RawRow, Reading } from "./types";

export interface FetchArgs {
  deviceId: string;
  /** inclusive lower bound on received_at (ISO) */
  sinceIso: string;
  /** exclusive lower bound: only rows newer than this (incremental poll) */
  afterIso?: string;
  /** exclusive upper bound: only rows older than this (paging backwards) */
  beforeIso?: string;
  ascending: boolean;
  limit?: number;
}

/** One bounded page. RLS limits this to the signed-in owner's rows. */
export async function fetchReadings(client: SupabaseClient, a: FetchArgs): Promise<Reading[]> {
  let q = client
    .from("fridge_readings")
    .select("*")
    .eq("device_id", a.deviceId)
    .gte("received_at", a.sinceIso);
  if (a.afterIso) q = q.gt("received_at", a.afterIso);
  if (a.beforeIso) q = q.lt("received_at", a.beforeIso);
  const { data, error } = await q.order("received_at", { ascending: a.ascending }).limit(a.limit ?? PAGE_SIZE);
  if (error) throw new Error(error.message);
  return ((data ?? []) as RawRow[]).map(normalizeRow);
}
